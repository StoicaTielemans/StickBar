#include "render/module_box.h"

#include <pango/pangocairo.h>

#include "config/config.h"

// Every module box render/measure call used to build a brand-new
// PangoFontDescription from scratch (allocating it, duplicating FONT.family's
// string, setting weight/size) and free it again immediately after. Since
// the whole bar redraws once a second (driven by the clock's own tick --
// see bar/bar_surface.c and main.c), that meant every widget on every output
// was doing this allocate/dup/free dance twice per frame (once in
// module_box_measure_text(), once again in whichever draw_text_* function
// followed it), forever, even though only a handful of distinct
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
        cairo_set_source_rgba(cr, fill.r, fill.g, fill.b, fill.a);
        cairo_fill(cr);
    }

    if (border_width > 0.0 && border.a > 0.0) {
        // Cairo strokes centered on the path, so inset by half the border width --
        // otherwise half the stroke would spill outside the (x,y,w,h) box.
        double inset = border_width / 2.0;
        cairo_rectangle(cr, x + inset, y + inset, w - border_width, h - border_width);
        cairo_set_source_rgba(cr, border.r, border.g, border.b, border.a);
        cairo_set_line_width(cr, border_width);
        cairo_stroke(cr);
    }

    cairo_restore(cr);
}

// Pango needs a cairo context to create a layout against for measuring, but
// nothing is ever actually drawn to it -- previously a fresh throwaway 1x1
// surface + context was created and destroyed on every single
// module_box_measure_text() call (every widget, every frame -- see the font
// cache's own doc comment above for why that adds up). A single 1x1 surface
// created once and kept alive for the rest of the process is just as valid
// to measure against (Pango's own metrics don't depend on the target
// surface's size) and removes that allocation from the hot path too.
static cairo_t *measure_context(void) {
    static cairo_surface_t *surface = NULL;
    static cairo_t *cr = NULL;
    if (!cr) {
        surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cr = cairo_create(surface);
    }
    return cr;
}

void module_box_measure_text(const char *text, int font_size_px, bool bold,
                              double *out_w, double *out_h) {
    PangoLayout *layout = pango_cairo_create_layout(measure_context());
    pango_layout_set_font_description(layout, cached_font(font_size_px, bold));
    pango_layout_set_text(layout, text, -1);

    int w, h;
    pango_layout_get_pixel_size(layout, &w, &h);
    if (out_w) *out_w = (double)w;
    if (out_h) *out_h = (double)h;

    g_object_unref(layout);
}

void module_box_measure_text_ink(const char *text, int font_size_px, bool bold,
                                  double *out_w, double *out_h) {
    PangoLayout *layout = pango_cairo_create_layout(measure_context());
    pango_layout_set_font_description(layout, cached_font(font_size_px, bold));
    pango_layout_set_text(layout, text, -1);

    PangoRectangle ink;
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    if (out_w) *out_w = (double)ink.width;
    if (out_h) *out_h = (double)ink.height;

    g_object_unref(layout);
}

void module_box_draw_text_centered(cairo_t *cr, double x, double y, double w, double h,
                                    const char *text, int font_size_px, bool bold, Color color) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, cached_font(font_size_px, bold));
    pango_layout_set_text(layout, text, -1);

    int text_w, text_h;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);

    cairo_save(cr);
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
    cairo_move_to(cr, x + (w - text_w) / 2.0, y + (h - text_h) / 2.0);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);

    g_object_unref(layout);
}

void module_box_draw_text_left(cairo_t *cr, double x, double y, double h,
                                const char *text, int font_size_px, bool bold, Color color) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, cached_font(font_size_px, bold));
    pango_layout_set_text(layout, text, -1);

    int text_w, text_h;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);
    (void)text_w; // only the height is needed for vertical centering here

    cairo_save(cr);
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
    cairo_move_to(cr, x, y + (h - text_h) / 2.0);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);

    g_object_unref(layout);
}
