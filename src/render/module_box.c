#include "render/module_box.h"

#include <pango/pangocairo.h>

#include "config/config.h"

static PangoFontDescription *build_font(int font_size_px, bool bold) {
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, FONT.family);
    pango_font_description_set_weight(desc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    // Absolute size in Pango units (1 px = PANGO_SCALE units) rather than points,
    // so this matches the pixel sizes from config.js's font.sizes.* directly.
    pango_font_description_set_absolute_size(desc, font_size_px * PANGO_SCALE);
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

void module_box_measure_text(const char *text, int font_size_px, bool bold,
                              double *out_w, double *out_h) {
    // Pango needs a cairo context to create a layout against, but nothing is ever
    // drawn to it -- a throwaway 1x1 surface is enough just to measure.
    cairo_surface_t *tmp_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *tmp_cr = cairo_create(tmp_surface);

    PangoLayout *layout = pango_cairo_create_layout(tmp_cr);
    PangoFontDescription *desc = build_font(font_size_px, bold);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, text, -1);

    int w, h;
    pango_layout_get_pixel_size(layout, &w, &h);
    if (out_w) *out_w = (double)w;
    if (out_h) *out_h = (double)h;

    pango_font_description_free(desc);
    g_object_unref(layout);
    cairo_destroy(tmp_cr);
    cairo_surface_destroy(tmp_surface);
}

void module_box_draw_text_centered(cairo_t *cr, double x, double y, double w, double h,
                                    const char *text, int font_size_px, bool bold, Color color) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = build_font(font_size_px, bold);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, text, -1);

    int text_w, text_h;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);

    cairo_save(cr);
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
    cairo_move_to(cr, x + (w - text_w) / 2.0, y + (h - text_h) / 2.0);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);

    pango_font_description_free(desc);
    g_object_unref(layout);
}

void module_box_draw_text_left(cairo_t *cr, double x, double y, double h,
                                const char *text, int font_size_px, bool bold, Color color) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = build_font(font_size_px, bold);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, text, -1);

    int text_w, text_h;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);
    (void)text_w; // only the height is needed for vertical centering here

    cairo_save(cr);
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
    cairo_move_to(cr, x, y + (h - text_h) / 2.0);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);

    pango_font_description_free(desc);
    g_object_unref(layout);
}
