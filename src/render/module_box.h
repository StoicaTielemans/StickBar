#ifndef CUSTOMBAR_RENDER_MODULE_BOX_H
#define CUSTOMBAR_RENDER_MODULE_BOX_H

#include <cairo.h>
#include <stdbool.h>

#include "config/config.h"

// Fills an (x, y, w, h) rect with `fill` and strokes a `border_width`-thick border
// in `border` around it. This is the shared "module box" chrome every bar widget
// (workspace pills, the clock, and future modules) uses for its background --
// individual widgets vary only in the colors/border width/size they pass in.
void module_box_draw_rect(cairo_t *cr, double x, double y, double w, double h,
                           Color fill, Color border, double border_width);

// Measures the pixel size `text` would occupy at the given font size/weight,
// without drawing anything. Used to size a module box to fit its label, mirroring
// the reference bar's `implicitWidth: label.implicitWidth + N` pattern.
void module_box_measure_text(const char *text, int font_size_px, bool bold,
                              double *out_w, double *out_h);

// Draws `text` centered within (x, y, w, h) in `color`, at the given font
// size/weight. Uses Pango (via pangocairo) rather than cairo's built-in toy text
// API so PUA icon glyphs and CJK kanji -- neither of which the toy API reliably
// shapes/substitutes correctly -- render properly through the Nerd Font.
void module_box_draw_text_centered(cairo_t *cr, double x, double y, double w, double h,
                                    const char *text, int font_size_px, bool bold, Color color);

#endif
