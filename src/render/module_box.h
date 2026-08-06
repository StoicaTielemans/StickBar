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
// the reference bar's `implicitWidth: label.implicitWidth + N` pattern. This is
// Pango's *logical* extent (roughly the cursor-advance width) -- for ordinary
// text and most icon glyphs this comfortably contains the glyph's own visible
// ink once this project's usual +12px module padding is added, but not
// always: see module_box_measure_text_ink() below for the one glyph (so far)
// where it doesn't.
void module_box_measure_text(const char *text, int font_size_px, bool bold,
                              double *out_w, double *out_h);

// Like module_box_measure_text(), but returns Pango's *ink* extent (the
// glyph's actual rendered pixels) instead of its logical/advance extent.
// Needed for modules/powermenu/arch_logo.c's Arch Linux logo glyph
// specifically: confirmed via a direct Pango probe that this glyph draws ink
// 14px wide at 14px-bold while its own logical/advance width is only 8px --
// a far larger ink-vs-advance gap than any other glyph in this project hits
// (most are within 2-3px, comfortably absorbed by the usual +12px pad).
// Sizing a box from the logical width alone left part of that glyph's own
// ink landing outside its box, invisible against the matching background
// color -- a real bug, not hypothetical (see arch_logo.c's own doc comment
// for the exact numbers and CLAUDE.md's "Power menu" section for the full
// writeup).
void module_box_measure_text_ink(const char *text, int font_size_px, bool bold,
                                  double *out_w, double *out_h);

// Draws `text` centered within (x, y, w, h) in `color`, at the given font
// size/weight. Uses Pango (via pangocairo) rather than cairo's built-in toy text
// API so PUA icon glyphs and CJK kanji -- neither of which the toy API reliably
// shapes/substitutes correctly -- render properly through the Nerd Font.
void module_box_draw_text_centered(cairo_t *cr, double x, double y, double w, double h,
                                    const char *text, int font_size_px, bool bold, Color color);

// Draws `text` left-aligned starting at `x`, vertically centered within a row
// spanning y..y+h. First needed by the tray context menu (DBusMenu entries are
// left-aligned, not centered, unlike every module box used so far).
void module_box_draw_text_left(cairo_t *cr, double x, double y, double h,
                                const char *text, int font_size_px, bool bold, Color color);

#endif
