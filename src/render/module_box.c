#include "render/module_box.h"

#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/config.h"
#include "render/color.h"

// Every module box render/measure call used to build a brand-new
// PangoFontDescription from scratch (allocating it, duplicating FONT.family's
// string, setting weight/size) and free it again immediately after -- twice
// per string per redraw (measure, then draw), even though only a handful of distinct
// (font_size_px, bold) combinations are ever actually used across this whole
// project (FONT.xs/sm/md/label/heading/title, each bold or not). Caching
// each one the first time it's requested and reusing the same
// PangoFontDescription* for the rest of the process's life removes that
// allocation from the hot path entirely -- safe because
// pango_layout_set_font_description() copies the description into the
// layout internally rather than taking ownership of the pointer we pass it
// (confirmed via Pango's own docs), so handing the same cached pointer to
// many different PangoLayouts, including ones that get destroyed on the very
// next line, never double-frees or leaves a layout holding a dangling
// pointer.
#define MAX_CACHED_FONT_DESCS 16

typedef struct {
    int font_size_px;
    bool bold;
    PangoFontDescription *desc;
} CachedFontDesc;

static CachedFontDesc g_font_cache[MAX_CACHED_FONT_DESCS];
static int g_font_cache_count = 0;

static PangoFontDescription *cached_font(int font_size_px, bool bold) {
    for (int i = 0; i < g_font_cache_count; i++) {
        if (g_font_cache[i].font_size_px == font_size_px && g_font_cache[i].bold == bold) {
            return g_font_cache[i].desc;
        }
    }

    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, FONT.family);
    pango_font_description_set_weight(desc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    // Absolute size in Pango units (1 px = PANGO_SCALE units) rather than points,
    // so this matches the pixel sizes from config.js's font.sizes.* directly.
    pango_font_description_set_absolute_size(desc, font_size_px * PANGO_SCALE);

    // If every distinct (size, bold) pair this project ever uses somehow grew
    // past MAX_CACHED_FONT_DESCS, the freshly built description above is
    // still returned and used correctly for this one call -- just not
    // cached for next time. Graceful degradation, not a bug: this project's
    // entire font-size surface (config.h's FontConfig) is 6 sizes, so 16
    // slots is already generous headroom, matching this codebase's usual
    // bounded-array convention.
    if (g_font_cache_count < MAX_CACHED_FONT_DESCS) {
        g_font_cache[g_font_cache_count++] = (CachedFontDesc){ font_size_px, bold, desc };
    }
    return desc;
}

void module_box_draw_rect(cairo_t *cr, double x, double y, double w, double h,
                           Color fill, Color border, double border_width) {
    cairo_save(cr);

    if (fill.a > 0.0) {
        cairo_rectangle(cr, x, y, w, h);
        color_set_source(cr, fill);
        cairo_fill(cr);
    }

    if (border_width > 0.0 && border.a > 0.0) {
        // Cairo strokes centered on the path, so inset by half the border width --
        // otherwise half the stroke would spill outside the (x,y,w,h) box.
        double inset = border_width / 2.0;
        cairo_rectangle(cr, x + inset, y + inset, w - border_width, h - border_width);
        color_set_source(cr, border);
        cairo_set_line_width(cr, border_width);
        cairo_stroke(cr);
    }

    cairo_restore(cr);
}

// --- Text: one shared context, and a cache of shaped layouts ---
//
// Every measure and every draw used to build a brand-new PangoContext +
// PangoLayout (pango_cairo_create_layout()), shape the string, and throw both
// away -- and nearly every caller measures a string and then draws the very
// same string, so each was shaped twice. A full bar redraw did that ~30-45
// times per output for only ~13 distinct strings, most of which don't change
// between redraws at all (labels, icons, unchanged numbers), and popups many
// more times per repaint. Now:
//
//   - One PangoContext for the whole process, made against a 1x1 ARGB32
//     image surface. Every real draw target is also an ARGB32 image surface
//     (the shm buffers, popup snapshots) with cairo's default font options,
//     so a layout shaped here renders identically on any of them -- which is
//     what made measuring against a separate surface correct all along.
//   - A small cache of shaped layouts keyed on (text, size, bold), evicting
//     the least recently used. Measure = lookup; draw = show the cached
//     layout. A string seen on the last redraw is never shaped again.
//
// Pango metrics don't depend on the measuring surface's size, so the 1x1
// surface is as good as any (it's never drawn to).
#define LAYOUT_CACHE_SIZE 64

typedef struct {
    char *text;           // owned; NULL = empty slot
    int font_size_px;
    bool bold;
    unsigned hash;
    PangoLayout *layout;
    int w, h;             // logical pixel size
    bool ink_valid;
    PangoRectangle ink;   // pixel ink extents, computed on first request
    unsigned long used;   // LRU clock
} CachedLayout;

static CachedLayout g_layouts[LAYOUT_CACHE_SIZE];
static unsigned long g_layout_clock = 0;

static PangoContext *shared_context(void) {
    static PangoContext *ctx = NULL;
    if (!ctx) {
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t *cr = cairo_create(surface);
        ctx = pango_cairo_create_context(cr);
        cairo_destroy(cr);
        cairo_surface_destroy(surface); // the context keeps what it needs (font options, resolution)
    }
    return ctx;
}

// FNV-1a over the text, mixed with size and weight -- just to skip most
// strcmp()s during lookup.
static unsigned layout_hash(const char *text, int font_size_px, bool bold) {
    unsigned h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        h = (h ^ *p) * 16777619u;
    }
    return (h ^ (unsigned)font_size_px * 31u) ^ (bold ? 0x9e3779b9u : 0u);
}

// The cached shaped layout for (text, size, bold), shaping it on a miss.
static CachedLayout *get_layout(const char *text, int font_size_px, bool bold) {
    unsigned hash = layout_hash(text, font_size_px, bold);
    CachedLayout *victim = &g_layouts[0];
    for (int i = 0; i < LAYOUT_CACHE_SIZE; i++) {
        CachedLayout *c = &g_layouts[i];
        if (c->text && c->hash == hash && c->font_size_px == font_size_px && c->bold == bold &&
            strcmp(c->text, text) == 0) {
            c->used = ++g_layout_clock;
            return c;
        }
        if (!c->text || c->used < victim->used) {
            victim = c; // an empty slot always wins (used = 0)
        }
    }

    if (!victim->layout) {
        victim->layout = pango_layout_new(shared_context());
    }
    free(victim->text);
    victim->text = strdup(text); // NULL on OOM: the slot is just never matched again
    victim->font_size_px = font_size_px;
    victim->bold = bold;
    victim->hash = hash;
    victim->ink_valid = false;
    victim->used = ++g_layout_clock;
    pango_layout_set_font_description(victim->layout, cached_font(font_size_px, bold));
    pango_layout_set_text(victim->layout, text, -1);
    pango_layout_get_pixel_size(victim->layout, &victim->w, &victim->h);
    return victim;
}

void module_box_measure_text(const char *text, int font_size_px, bool bold,
                              double *out_w, double *out_h) {
    CachedLayout *c = get_layout(text, font_size_px, bold);
    if (out_w) *out_w = (double)c->w;
    if (out_h) *out_h = (double)c->h;
}

double module_box_text_width(const char *text, int font_size_px, bool bold) {
    return (double)get_layout(text, font_size_px, bold)->w;
}

void module_box_measure_text_ink(const char *text, int font_size_px, bool bold,
                                  double *out_w, double *out_h) {
    CachedLayout *c = get_layout(text, font_size_px, bold);
    if (!c->ink_valid) {
        pango_layout_get_pixel_extents(c->layout, &c->ink, NULL);
        c->ink_valid = true;
    }
    if (out_w) *out_w = (double)c->ink.width;
    if (out_h) *out_h = (double)c->ink.height;
}

// Draws a cached layout with its top-left at (x, y).
static void show_layout(cairo_t *cr, const CachedLayout *c, double x, double y, Color color) {
    cairo_save(cr);
    color_set_source(cr, color);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, c->layout);
    cairo_restore(cr);
}

void module_box_draw_text_centered(cairo_t *cr, double x, double y, double w, double h,
                                    const char *text, int font_size_px, bool bold, Color color) {
    CachedLayout *c = get_layout(text, font_size_px, bold);
    show_layout(cr, c, x + (w - c->w) / 2.0, y + (h - c->h) / 2.0, color);
}

void module_box_draw_text_left(cairo_t *cr, double x, double y, double h,
                                const char *text, int font_size_px, bool bold, Color color) {
    CachedLayout *c = get_layout(text, font_size_px, bold);
    show_layout(cr, c, x, y + (h - c->h) / 2.0, color);
}

void module_box_draw_text_right(cairo_t *cr, double right_x, double y, double h,
                                 const char *text, int font_size_px, bool bold, Color color) {
    CachedLayout *c = get_layout(text, font_size_px, bold);
    show_layout(cr, c, right_x - c->w, y + (h - c->h) / 2.0, color);
}

// --- Ellipsis truncation ---
//
// Was implemented four times (media box, media/device/mixer dropdowns), each
// with a fixed-size array of codepoint offsets and a fixed candidate buffer
// that very long input could overrun into a mid-codepoint cut. This one walks
// codepoints with g_utf8 and builds candidates in a buffer sized from the
// input. The binary search measures with a private scratch layout (so its
// ~log2(n) throwaway candidates don't flush the layout cache), and the result
// is memoised: callers re-truncate the same titles on every repaint.
#define TRUNCATE_CACHE_SIZE 16

typedef struct {
    char *text;
    char *result;
    double max_w;
    int font_size_px;
    bool bold;
    unsigned long used;
} CachedTruncation;

static CachedTruncation g_truncations[TRUNCATE_CACHE_SIZE];
static unsigned long g_truncation_clock = 0;

static int scratch_width(const char *text, int font_size_px, bool bold) {
    static PangoLayout *scratch = NULL;
    if (!scratch) {
        scratch = pango_layout_new(shared_context());
    }
    pango_layout_set_font_description(scratch, cached_font(font_size_px, bold));
    pango_layout_set_text(scratch, text, -1);
    int w, h;
    pango_layout_get_pixel_size(scratch, &w, &h);
    return w;
}

// The uncached search: the longest codepoint prefix of `text` that fits in
// `max_w` with "..." appended. Returns a malloc'd string.
static char *truncate_uncached(const char *text, double max_w, int font_size_px, bool bold) {
    if (module_box_text_width(text, font_size_px, bold) <= max_w) {
        return strdup(text);
    }
    size_t len = strlen(text);
    char *candidate = malloc(len + 4);
    if (!candidate) {
        return strdup("...");
    }
    long lo = 0, hi = g_utf8_strlen(text, (gssize)len);
    while (lo < hi) {
        long mid = (lo + hi + 1) / 2;
        size_t bytes = (size_t)(g_utf8_offset_to_pointer(text, mid) - text);
        memcpy(candidate, text, bytes);
        memcpy(candidate + bytes, "...", 4);
        if (scratch_width(candidate, font_size_px, bold) <= max_w) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    size_t bytes = (size_t)(g_utf8_offset_to_pointer(text, lo) - text);
    memcpy(candidate, text, bytes);
    memcpy(candidate + bytes, "...", 4);
    return candidate;
}

void module_box_truncate_to_width(const char *text, double max_w, int font_size_px, bool bold,
                                   char *out, size_t out_size) {
    if (out_size == 0) {
        return;
    }
    CachedTruncation *hit = NULL, *victim = &g_truncations[0];
    for (int i = 0; i < TRUNCATE_CACHE_SIZE; i++) {
        CachedTruncation *c = &g_truncations[i];
        if (c->text && c->max_w == max_w && c->font_size_px == font_size_px && c->bold == bold &&
            strcmp(c->text, text) == 0) {
            hit = c;
            break;
        }
        if (!c->text || c->used < victim->used) {
            victim = c;
        }
    }
    if (!hit) {
        char *result = truncate_uncached(text, max_w, font_size_px, bold);
        char *copy = strdup(text);
        if (!result || !copy) {
            free(result);
            free(copy);
            snprintf(out, out_size, "%s", text);
            return;
        }
        free(victim->text);
        free(victim->result);
        *victim = (CachedTruncation){copy, result, max_w, font_size_px, bold, 0};
        hit = victim;
    }
    hit->used = ++g_truncation_clock;

    // Copy out, never cutting a codepoint in half if `out` is smaller than
    // the result.
    size_t n = strlen(hit->result);
    if (n >= out_size) {
        const char *end = g_utf8_find_prev_char(hit->result, hit->result + out_size);
        n = end ? (size_t)(end - hit->result) : 0;
    }
    memcpy(out, hit->result, n);
    out[n] = '\0';
}

double module_box_label(cairo_t *cr, BoxRectTable *rects, Output *output, ModuleBoxAnchor anchor, double edge_x,
                        double box_top, double box_height, const char *label, Color accent) {
    double box_w = module_box_text_width(label, FONT.label, true) + BAR.module_padding;
    double box_x = anchor == MODULE_BOX_FROM_LEFT ? edge_x : edge_x - box_w;
    module_box_draw_rect(cr, box_x, box_top, box_w, box_height, COLORS.mantle, accent,
                         (double)BAR.module_border_width);
    module_box_draw_text_centered(cr, box_x, box_top, box_w, box_height, label, FONT.label, true, accent);
    box_rect_set(rects, output, box_x, box_top, box_w, box_height);
    return anchor == MODULE_BOX_FROM_LEFT ? box_x + box_w + BAR.item_spacing : box_x - BAR.item_spacing;
}
