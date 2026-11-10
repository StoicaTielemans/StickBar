#ifndef CUSTOMBAR_RENDER_POPUP_CHROME_H
#define CUSTOMBAR_RENDER_POPUP_CHROME_H

#include <cairo.h>
#include <stdbool.h>

#include "config/config.h"

// The chrome every bar dropdown draws the same way (docs/popups.md "Look"):
// the accent-bordered frame, the header row with its status pill, and cards.
// Each dropdown used to paint these itself, ten-odd times over; the layout
// tokens they use are in render/popup_style.h.

// The whole popup's background: `mantle`, with a BAR.module_border_width
// border in the dropdown's accent.
void popup_draw_frame(cairo_t *cr, double w, double h, Color accent);

// A filled status pill whose right edge is at `right_x`, vertically centered
// in a `row_h` row starting at `row_y`: `word` in bold FONT.xs, `mantle` on
// `fill`.
double popup_pill_width(const char *word);
void popup_draw_pill(cairo_t *cr, double right_x, double row_y, double row_h, const char *word, Color fill);

typedef struct {
    const char *icon;    // UTF-8 glyph (from glyph_utf8()), drawn in icon_color at FONT.heading
    Color icon_color;
    const char *title;   // bold FONT.md, `text`
    const char *summary; // FONT.sm, `subtext0`, right after the title; NULL/"" = none
    const char *pill;    // right-aligned status pill; NULL = none
    Color pill_fill;
} PopupHeader;

// The header row: icon, title, summary, and the pill at the right end of a
// `w`-wide row of height POPUP_HEADER_H at (x, y). Text that would run into the
// pill is ellipsized (the summary first, then the title), so a long device or
// preset name can never draw over it.
void popup_draw_header(cairo_t *cr, double x, double y, double w, const PopupHeader *header);

// The width the header needs to show everything unshortened -- for dropdowns
// that size themselves to their content.
double popup_header_width(const PopupHeader *header);

// A card: `base` fill and a 1px `surface1` border, or for the current item
// (active player, default device, current month, active profile) a 2px
// `accent` border; `hovered` lightens the fill to `surface0`.
void popup_draw_card(cairo_t *cr, double x, double y, double w, double h, bool current, bool hovered, Color accent);

#endif
