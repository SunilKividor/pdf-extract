/*
 * pdf_logger.cpp
 *
 * Intercepts MuPDF's display list replay and dumps ALL draw actions to JSON.
 * Also saves embedded images to disk as PNG files.
 *
 * Build:
 *   g++ -std=c++17 main.cpp \
 *       -I/path/to/mupdf/include \
 *       -L/path/to/mupdf/build/release \
 *       -lmupdf -lmupdf-third \
 *       -o pdf_logger
 *
 * Run:
 *   ./pdf_logger input.pdf          → writes actions_page0.json + image_NNN.png
 *   ./pdf_logger input.pdf 2        → page index 2 (0-based)
 *
 * ALL output goes to files in the CURRENT DIRECTORY.
 * Progress/errors go to stderr so you can see them in the terminal.
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>


int g_current_page = 0;
char g_output_dir[512];

extern "C" {
#include <mupdf/fitz.h>
}

/* ============================================================
   GLOBALS
   ============================================================ */

static int   g_action_index = 0;
static int   g_image_index  = 0;
static bool  g_first_action = true;
static FILE* g_out          = nullptr;  // points to actions_pageN.json


/* ============================================================
   MINIMAL JSON HELPERS
   ============================================================ */

static void json_begin() { fprintf(g_out, "[\n"); }
static void json_end()   { fprintf(g_out, "\n]\n"); }

static std::string json_str(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (unsigned char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20)  { char buf[8]; snprintf(buf,8,"\\u%04x",c); out+=buf; }
        else                out += c;
    }
    out += '"';
    return out;
}

static void action_start(const char* type)
{
    if (!g_first_action) fprintf(g_out, ",\n");
    g_first_action = false;
    fprintf(g_out, "  {\n");
    fprintf(g_out, "    \"index\": %d,\n",    g_action_index++);
    fprintf(g_out, "    \"type\": \"%s\",\n", type);
}

static void action_end() { fprintf(g_out, "  }"); }

static void print_bbox(fz_rect r, const char* name = "bbox")
{
    fprintf(g_out,
        "    \"%s\": { \"x0\": %.4f, \"y0\": %.4f, \"x1\": %.4f, \"y1\": %.4f },\n",
        name, r.x0, r.y0, r.x1, r.y1);
    fprintf(g_out,
        "    \"%s_size\": { \"w\": %.4f, \"h\": %.4f },\n",
        name, r.x1 - r.x0, r.y1 - r.y0);
}

static void print_color(fz_colorspace* cs, const float* color, float alpha)
{
    if (cs && color) {
        const char* csname = fz_colorspace_name(NULL, cs);
        int n = fz_colorspace_n(NULL, cs);
        fprintf(g_out, "    \"colorspace\": \"%s\",\n", csname ? csname : "unknown");
        fprintf(g_out, "    \"color\": [");
        for (int i = 0; i < n; i++)
            fprintf(g_out, "%.4f%s", color[i], (i < n-1) ? ", " : "");
        fprintf(g_out, "],\n");
    }
    fprintf(g_out, "    \"alpha\": %.4f,\n", alpha);
}


/* ============================================================
   PATH WALKER
   ============================================================ */

struct PathWalker {
    std::vector<std::string> segments;
    float cur_x = 0, cur_y = 0;

    void moveto(float x, float y) {
        char buf[128];
        snprintf(buf, sizeof(buf),
            "{\"op\":\"moveto\",\"x\":%.4f,\"y\":%.4f}", x, y);
        segments.push_back(buf);
        cur_x = x; cur_y = y;
    }
    void lineto(float x, float y) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"op\":\"lineto\","
            "\"from\":{\"x\":%.4f,\"y\":%.4f},"
            "\"to\":{\"x\":%.4f,\"y\":%.4f},"
            "\"dx\":%.4f,\"dy\":%.4f}",
            cur_x, cur_y, x, y, x - cur_x, y - cur_y);
        segments.push_back(buf);
        cur_x = x; cur_y = y;
    }
    void curveto(float x1,float y1,float x2,float y2,float x3,float y3) {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"op\":\"curveto\","
            "\"cp1\":{\"x\":%.4f,\"y\":%.4f},"
            "\"cp2\":{\"x\":%.4f,\"y\":%.4f},"
            "\"to\":{\"x\":%.4f,\"y\":%.4f}}",
            x1,y1, x2,y2, x3,y3);
        segments.push_back(buf);
        cur_x = x3; cur_y = y3;
    }
    void closepath() { segments.push_back("{\"op\":\"closepath\"}"); }
    void rect(float x, float y, float w, float h) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"op\":\"rect\","
            "\"x\":%.4f,\"y\":%.4f,\"w\":%.4f,\"h\":%.4f}",
            x, y, w, h);
        segments.push_back(buf);
    }

    void print_json() const {
        fprintf(g_out, "    \"segments\": [\n");
        for (size_t i = 0; i < segments.size(); i++)
            fprintf(g_out, "      %s%s\n",
                segments[i].c_str(), (i+1 < segments.size()) ? "," : "");
        fprintf(g_out, "    ],\n");
    }
};

static void pw_moveto(fz_context*, void* arg, float x, float y)
{ ((PathWalker*)arg)->moveto(x,y); }

static void pw_lineto(fz_context*, void* arg, float x, float y)
{ ((PathWalker*)arg)->lineto(x,y); }

static void pw_curveto(fz_context*, void* arg,
    float x1,float y1,float x2,float y2,float x3,float y3)
{ ((PathWalker*)arg)->curveto(x1,y1,x2,y2,x3,y3); }

static void pw_closepath(fz_context*, void* arg)
{ ((PathWalker*)arg)->closepath(); }

static void pw_rectto(fz_context*, void* arg,
    float x0, float y0, float x1, float y1)
{ ((PathWalker*)arg)->rect(x0, y0, x1-x0, y1-y0); }

static PathWalker walk_path(fz_context* ctx, const fz_path* path)
{
    static const fz_path_walker vtable = {
        pw_moveto, pw_lineto, pw_curveto, pw_closepath, pw_rectto
    };
    PathWalker pw;
    fz_walk_path(ctx, path, &vtable, &pw);
    return pw;
}


/* ============================================================
   TEXT HELPERS
   ============================================================ */

static std::string collect_text(const fz_text* text)
{
    std::string result;
    for (fz_text_span* span = text->head; span != nullptr; span = span->next) {
        for (int i = 0; i < span->len; i++) {
            int cp = span->items[i].ucs;
            if      (cp < 0x80)    { result += (char)cp; }
            else if (cp < 0x800)   { result += (char)(0xC0|(cp>>6));  result += (char)(0x80|(cp&0x3F)); }
            else if (cp < 0x10000) { result += (char)(0xE0|(cp>>12)); result += (char)(0x80|((cp>>6)&0x3F));  result += (char)(0x80|(cp&0x3F)); }
            else                   { result += (char)(0xF0|(cp>>18)); result += (char)(0x80|((cp>>12)&0x3F)); result += (char)(0x80|((cp>>6)&0x3F)); result += (char)(0x80|(cp&0x3F)); }
        }
    }
    return result;
}

static void print_text_chars(const fz_text* text)
{
    fprintf(g_out, "    \"chars\": [\n");
    bool first = true;
    for (fz_text_span* span = text->head; span != nullptr; span = span->next) {
        fz_matrix trm = span->trm;
        float font_size = std::sqrt(trm.a * trm.a + trm.b * trm.b);
        for (int i = 0; i < span->len; i++) {
            if (!first) fprintf(g_out, ",\n");
            first = false;
            int   cp  = span->items[i].ucs;
            float x   = span->items[i].x;
            float y   = span->items[i].y;
            float adv = span->items[i].adv;
            int   gid = span->items[i].gid;
            std::string ch = (cp >= 32 && cp < 127) ? std::string(1,(char)cp) : "?";
            fprintf(g_out,
                "      {\"ucs\":%d,\"char\":%s,\"gid\":%d,"
                "\"origin\":{\"x\":%.4f,\"y\":%.4f},"
                "\"advance\":%.4f,\"font_size\":%.4f}",
                cp, json_str(ch).c_str(), gid, x, y, adv, font_size);
        }
    }
    fprintf(g_out, "\n    ],\n");
}


/* ============================================================
   DEVICE STRUCT
   ============================================================ */

typedef struct {
    fz_device base;   /* MUST be first member */
} logger_device;


/* ============================================================
   LOGGER CALLBACKS
   ============================================================ */

static void my_fill_text(
    fz_context* ctx, fz_device* dev_,
    const fz_text* text, fz_matrix ctm,
    fz_colorspace* cs, const float* color, float alpha, fz_color_params cp)
{
    fz_rect bbox = fz_bound_text(ctx, text, NULL, ctm);
    std::string txt = collect_text(text);
    action_start("fill_text");
    print_bbox(bbox);
    print_color(cs, color, alpha);
    fprintf(g_out, "    \"text\": %s,\n", json_str(txt).c_str());
    print_text_chars(text);
    fprintf(g_out, "    \"ctm\": [%.4f,%.4f,%.4f,%.4f,%.4f,%.4f]\n",
        ctm.a, ctm.b, ctm.c, ctm.d, ctm.e, ctm.f);
    action_end();
}

static void my_stroke_path(
    fz_context* ctx, fz_device* dev_,
    const fz_path* path, const fz_stroke_state* stroke,
    fz_matrix ctm, fz_colorspace* cs,
    const float* color, float alpha, fz_color_params cp)
{
    fz_rect bbox = fz_bound_path(ctx, path, stroke, ctm);
    PathWalker pw = walk_path(ctx, path);
    action_start("stroke_path");
    print_bbox(bbox);
    print_color(cs, color, alpha);
    if (stroke) {
        fprintf(g_out, "    \"line_width\": %.4f,\n", stroke->linewidth);
        fprintf(g_out, "    \"line_cap\": %d,\n",     stroke->start_cap);
        fprintf(g_out, "    \"line_join\": %d,\n",    stroke->linejoin);
        fprintf(g_out, "    \"miter_limit\": %.4f,\n",stroke->miterlimit);
    }
    pw.print_json();
    fprintf(g_out, "    \"ctm\": [%.4f,%.4f,%.4f,%.4f,%.4f,%.4f]\n",
        ctm.a, ctm.b, ctm.c, ctm.d, ctm.e, ctm.f);
    action_end();
}

static void my_fill_path(
    fz_context* ctx, fz_device* dev_,
    const fz_path* path, int even_odd,
    fz_matrix ctm, fz_colorspace* cs,
    const float* color, float alpha, fz_color_params cp)
{
    fz_rect bbox = fz_bound_path(ctx, path, NULL, ctm);
    PathWalker pw = walk_path(ctx, path);
    action_start("fill_path");
    print_bbox(bbox);
    print_color(cs, color, alpha);
    fprintf(g_out, "    \"fill_rule\": \"%s\",\n", even_odd ? "even_odd" : "nonzero");
    pw.print_json();
    fprintf(g_out, "    \"ctm\": [%.4f,%.4f,%.4f,%.4f,%.4f,%.4f]\n",
        ctm.a, ctm.b, ctm.c, ctm.d, ctm.e, ctm.f);
    action_end();
}

static void my_fill_image(
    fz_context* ctx, fz_device* dev_,
    fz_image* image, fz_matrix ctm,
    float alpha, fz_color_params cp)
{
    int w = image->w;
    int h = image->h;
    int n = image->n;

    /* unit rect [0,0,1,1] transformed by CTM = where image lands on the page */
    fz_rect page_rect = fz_transform_rect(fz_unit_rect, ctm);

    char img_filename[512];
    snprintf(
        img_filename,
        sizeof(img_filename),
        "%s/page_%d/image_%03d.png",
        g_output_dir,
        g_current_page,
        g_image_index
    );

    fz_pixmap* pix = nullptr;
    const char* save_error = nullptr;
    fz_try(ctx) {
        pix = fz_get_pixmap_from_image(ctx, image, NULL, NULL, NULL, NULL);
        fz_save_pixmap_as_png(ctx, pix, img_filename);
    }
    fz_catch(ctx) {
        save_error = fz_caught_message(ctx);
    }
    if (pix) fz_drop_pixmap(ctx, pix);

    action_start("fill_image");
    fprintf(g_out, "    \"image_index\": %d,\n",     g_image_index++);
    fprintf(g_out, "    \"pixel_size\": {\"w\":%d,\"h\":%d},\n", w, h);
    fprintf(g_out, "    \"components\": %d,\n",       n);
    fprintf(g_out, "    \"alpha\": %.4f,\n",           alpha);
    print_bbox(page_rect, "page_bbox");
    fprintf(g_out, "    \"saved_to\": \"%s\",\n",     img_filename);
    if (save_error)
        fprintf(g_out, "    \"save_error\": \"%s\",\n", save_error);
    fprintf(g_out, "    \"ctm\": [%.4f,%.4f,%.4f,%.4f,%.4f,%.4f]\n",
        ctm.a, ctm.b, ctm.c, ctm.d, ctm.e, ctm.f);
    action_end();
}

static void my_fill_image_mask(
    fz_context* ctx, fz_device* dev_,
    fz_image* image, fz_matrix ctm,
    fz_colorspace* cs, const float* color, float alpha, fz_color_params cp)
{
    fz_rect page_rect = fz_transform_rect(fz_unit_rect, ctm);
    action_start("fill_image_mask");
    fprintf(g_out, "    \"pixel_size\": {\"w\":%d,\"h\":%d},\n", image->w, image->h);
    print_color(cs, color, alpha);
    print_bbox(page_rect, "page_bbox");
    fprintf(g_out, "    \"ctm\": [%.4f,%.4f,%.4f,%.4f,%.4f,%.4f]\n",
        ctm.a, ctm.b, ctm.c, ctm.d, ctm.e, ctm.f);
    action_end();
}

static void my_clip_path(
    fz_context* ctx, fz_device* dev_,
    const fz_path* path, int even_odd, fz_matrix ctm, fz_rect scissor)
{
    fz_rect bbox = fz_bound_path(ctx, path, NULL, ctm);
    PathWalker pw = walk_path(ctx, path);
    action_start("clip_path");
    print_bbox(bbox);
    fprintf(g_out, "    \"fill_rule\": \"%s\",\n", even_odd ? "even_odd" : "nonzero");
    pw.print_json();
    fprintf(g_out, "    \"ctm\": [%.4f,%.4f,%.4f,%.4f,%.4f,%.4f]\n",
        ctm.a, ctm.b, ctm.c, ctm.d, ctm.e, ctm.f);
    action_end();
}

static void my_pop_clip(fz_context* ctx, fz_device* dev_)
{
    action_start("pop_clip");
    fprintf(g_out, "    \"note\": \"end of clipping region\"\n");
    action_end();
}


/* ============================================================
   CREATE LOGGER DEVICE
   ============================================================ */

fz_device* make_logger_device(fz_context* ctx)
{
    logger_device* dev = fz_new_derived_device(ctx, logger_device);
    dev->base.fill_text        = my_fill_text;
    dev->base.stroke_path      = my_stroke_path;
    dev->base.fill_path        = my_fill_path;
    dev->base.fill_image       = my_fill_image;
    dev->base.fill_image_mask  = my_fill_image_mask;
    dev->base.clip_path        = my_clip_path;
    dev->base.pop_clip         = my_pop_clip;
    return &dev->base;
}


/* ============================================================
   MAIN
   ============================================================ */

// int main(int argc, char **argv)
// {
//     if (argc < 2) {
//         fprintf(stderr, "usage: ./pdf_logger file.pdf [page_number]\n");
//         fprintf(stderr, "  writes: actions_pageN.json  (current directory)\n");
//         fprintf(stderr, "  writes: image_NNN.png        (current directory)\n");
//         return 1;
//     }

//     const char* filename  = argv[1];
//     int target_page = (argc >= 3) ? atoi(argv[2]) : 0;  // 0-indexed

//     /* ── Open output JSON file FIRST ── */
//     char json_filename[256];
//     snprintf(json_filename, sizeof(json_filename), "actions_page%d.json", target_page);
//     g_out = fopen(json_filename, "w");
//     if (!g_out) {
//         fprintf(stderr, "ERROR: cannot create '%s' — check write permissions\n", json_filename);
//         return 1;
//     }
//     fprintf(stderr, "Output JSON  : %s\n", json_filename);
//     fprintf(stderr, "Output images: image_NNN.png (same directory)\n");

//     /* ── MuPDF init ── */
//     fz_context* ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
//     if (!ctx) {
//         fprintf(stderr, "ERROR: cannot create mupdf context\n");
//         fclose(g_out);
//         return 1;
//     }
//     fz_register_document_handlers(ctx);

//     /* ── Open document ── */
//     fz_document* doc = nullptr;
//     fz_try(ctx) {
//         doc = fz_open_document(ctx, filename);
//     }
//     fz_catch(ctx) {
//         fprintf(stderr, "ERROR: cannot open '%s': %s\n", filename, fz_caught_message(ctx));
//         fclose(g_out);
//         fz_drop_context(ctx);
//         return 1;
//     }

//     int page_count = fz_count_pages(ctx, doc);
//     fprintf(stderr, "Document     : %s\n", filename);
//     fprintf(stderr, "Pages        : %d\n", page_count);
//     fprintf(stderr, "Processing   : page %d (0-based)\n", target_page);

//     if (target_page < 0 || target_page >= page_count) {
//         fprintf(stderr, "ERROR: page %d out of range (valid: 0-%d)\n",
//             target_page, page_count - 1);
//         fclose(g_out);
//         fz_drop_document(ctx, doc);
//         fz_drop_context(ctx);
//         return 1;
//     }

//     /* ── Load page + build display list ── */
//     fz_page* page   = fz_load_page(ctx, doc, target_page);
//     fz_rect  bounds = fz_bound_page(ctx, page);
//     fprintf(stderr, "Page bounds  : (%.2f, %.2f, %.2f, %.2f) pts\n",
//         bounds.x0, bounds.y0, bounds.x1, bounds.y1);

//     fz_display_list* list = fz_new_display_list(ctx, bounds);
//     {
//         fz_device* list_dev = fz_new_list_device(ctx, list);
//         fz_run_page(ctx, page, list_dev, fz_identity, NULL);
//         fz_close_device(ctx, list_dev);
//         fz_drop_device(ctx, list_dev);
//     }
//     fprintf(stderr, "Display list : built — replaying...\n");

//     /* ── Replay into logger ── */
//     json_begin();
//     fz_device* logger = make_logger_device(ctx);
//     fz_try(ctx) {
//         fz_run_display_list(ctx, list, logger, fz_identity, fz_infinite_rect, NULL);
//     }
//     fz_catch(ctx) {
//         fprintf(stderr, "WARNING: replay error: %s\n", fz_caught_message(ctx));
//     }
//     fz_close_device(ctx, logger);
//     fz_drop_device(ctx, logger);
//     json_end();

//     /* ── Close JSON file ── */
//     fclose(g_out);
//     g_out = nullptr;

//     fprintf(stderr, "─────────────────────────────────────────\n");
//     fprintf(stderr, "  Actions logged : %d\n",    g_action_index);
//     fprintf(stderr, "  Images saved   : %d\n",    g_image_index);
//     fprintf(stderr, "  JSON written   : %s\n",    json_filename);
//     fprintf(stderr, "─────────────────────────────────────────\n");

//     /* ── Cleanup ── */
//     fz_drop_display_list(ctx, list);
//     fz_drop_page(ctx, page);
//     fz_drop_document(ctx, doc);
//     fz_drop_context(ctx);

//     return 0;
// }

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: ./pdf_logger file.pdf output_dir\n");
        fprintf(stderr, "example: ./pdf_logger planogram.pdf output\n");
        return 1;
    }

    const char* filename = argv[1];
    const char* out_dir  = argv[2];

    strcpy(g_output_dir, out_dir);

    mkdir(out_dir, 0777);

    /* ── MuPDF init ── */

    fz_context* ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);

    if (!ctx) {
        fprintf(stderr, "ERROR: cannot create mupdf context\n");
        return 1;
    }

    fz_register_document_handlers(ctx);

    /* ── Open document ── */

    fz_document* doc = nullptr;

    fz_try(ctx) {
        doc = fz_open_document(ctx, filename);
    }
    fz_catch(ctx) {
        fprintf(stderr, "ERROR: cannot open '%s': %s\n", filename, fz_caught_message(ctx));
        fz_drop_context(ctx);
        return 1;
    }

    int page_count = fz_count_pages(ctx, doc);

    fprintf(stderr, "Document : %s\n", filename);
    fprintf(stderr, "Pages    : %d\n", page_count);
    fprintf(stderr, "Output   : %s\n", out_dir);

    /* ───────────────────────────── */
    /* Process all pages             */
    /* ───────────────────────────── */

    for (int p = 0; p < page_count; p++)
    {
        g_current_page = p;

        g_action_index = 0;
        g_image_index  = 0;
        g_first_action = true;

        char page_folder[512];
        snprintf(page_folder, sizeof(page_folder), "%s/page_%d", out_dir, p);

        mkdir(page_folder, 0777);

        char json_filename[512];
        snprintf(json_filename, sizeof(json_filename), "%s/actions.json", page_folder);

        g_out = fopen(json_filename, "w");

        if (!g_out) {
            fprintf(stderr, "ERROR: cannot create '%s'\n", json_filename);
            continue;
        }

        fprintf(stderr, "\nProcessing page %d\n", p);
        fprintf(stderr, "JSON : %s\n", json_filename);

        /* ── Load page ── */

        fz_page* page = fz_load_page(ctx, doc, p);

        fz_rect bounds = fz_bound_page(ctx, page);

        fprintf(stderr,
            "Bounds: (%.2f %.2f %.2f %.2f)\n",
            bounds.x0, bounds.y0, bounds.x1, bounds.y1
        );

        /* ── Build display list ── */

        fz_display_list* list = fz_new_display_list(ctx, bounds);

        {
            fz_device* list_dev = fz_new_list_device(ctx, list);

            fz_run_page(ctx, page, list_dev, fz_identity, NULL);

            fz_close_device(ctx, list_dev);
            fz_drop_device(ctx, list_dev);
        }

        /* ── Replay display list ── */

        json_begin();

        fz_device* logger = make_logger_device(ctx);

        fz_try(ctx) {
            fz_run_display_list(ctx, list, logger, fz_identity, fz_infinite_rect, NULL);
        }
        fz_catch(ctx) {
            fprintf(stderr, "WARNING: replay error: %s\n", fz_caught_message(ctx));
        }

        fz_close_device(ctx, logger);
        fz_drop_device(ctx, logger);

        json_end();

        fclose(g_out);
        g_out = nullptr;

        fprintf(stderr, "Actions logged : %d\n", g_action_index);
        fprintf(stderr, "Images saved   : %d\n", g_image_index);

        /* ── Cleanup page resources ── */

        fz_drop_display_list(ctx, list);
        fz_drop_page(ctx, page);
    }

    /* ── Cleanup document ── */

    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);

    fprintf(stderr, "\nAll pages processed successfully\n");

    return 0;
}