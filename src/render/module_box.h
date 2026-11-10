#ifndef CUSTOMBAR_RENDER_MODULE_BOX_H
#define CUSTOMBAR_RENDER_MODULE_BOX_H

#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>

#include "config/config.h"
#include "render/box_rects.h"

// Fills an (x, y, w, h) rect with `fill` and strokes a `border_width`-thick border
// in `border` around it. This is the shared "module box" chrome every bar widget
// and popup card uses for its background -- callers vary only in the
// colors/border width/size they pass in.
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
// All text goes through a cache of shaped Pango layouts (see module_box.c):
// measuring and then drawing the same string shapes it once, and strings
// unchanged since the last redraw aren't shaped again at all.
void module_box_measure_text(const char *text, int font_size_px, bool bold,
                              double *out_w, double *out_h);

// Just the logical width -- what nearly every caller wants.
double module_box_text_width(const char *text, int font_size_px, bool bold);

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
// for the exact numbers and BUG.md's "Arch logo cut off" for the full
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

// Draws `text` right-aligned so it ends at `right_x`, vertically centered
// within y..y+h.
void module_box_draw_text_right(cairo_t *cr, double right_x, double y, double h,
                                 const char *text, int font_size_px, bool bold, Color color);

// Copies `text` into `out`, cut at a codepoint boundary with "..." appended
// if it's wider than `max_w` at the given font -- the longest such prefix
// (binary search). Never cuts a multi-byte UTF-8 sequence (titles and device
// names routinely have them; pango drops invalid UTF-8 with a warning).
// Results are memoised, so re-truncating the same string every repaint is
// cheap.
void module_box_truncate_to_width(const char *text, double max_w, int font_size_px, bool bold,
                                   char *out, size_t out_size);

// Which edge a bar box is laid out from: left-region modules are placed left
// to right from the x they're given; right-region ones right to left, ending
// at the x they're given.
typedef enum {
    MODULE_BOX_FROM_LEFT,
    MODULE_BOX_FROM_RIGHT,
} ModuleBoxAnchor;

// The standard bar box: `label` (bold FONT.label) centered in a box
// BAR.module_padding wider than it, `mantle` fill and a BAR.module_border_width
// border in `accent`, recorded in `rects` for `output`. Returns where the
// next module goes (BAR.item_spacing past this box's far edge). Most bar
// modules are exactly this plus building their label.
double module_box_label(cairo_t *cr, BoxRectTable *rects, Output *output, ModuleBoxAnchor anchor, double edge_x,
                        double box_top, double box_height, const char *label, Color accent);

#endif
