#ifndef CUSTOMBAR_RENDER_BRAILLE_GRAPH_H
#define CUSTOMBAR_RENDER_BRAILLE_GRAPH_H

#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>

#include "config/config.h"

/*
 * btop-style "braille" history graph: a grid of character-sized cells, each
 * 2 dots wide and 4 dots tall, with one sample per dot column and the dots
 * coloured by how high they sit. Pure cairo, no data-source types (same
 * plumbing/rendering split as eq/eq_curve.c), so any popup with a history
 * of numbers can use it -- the network dropdown is the first.
 *
 * The dots are drawn as small cairo circles on the braille geometry rather
 * than as braille *characters* (U+2800..U+28FF, which the Nerd Font does
 * have): drawing them gives crisp, evenly spaced dots at any size and lets
 * every dot row have its own colour, which would otherwise need one pango
 * layout per cell.
 */

// Pixel size of a graph `cells_w` cells wide / `cells_h` cells tall.
double braille_graph_width(int cells_w);
double braille_graph_height(int cells_h);

// The most whole cells that fit in `width` pixels -- for a graph that fills
// its card instead of sitting centred in it at a fixed size.
int braille_graph_cells_for_width(double width);

// The left end of the time axis under a graph `cells_w` cells wide, one
// sample per dot column every `seconds_per_sample`: "8 min ago".
void braille_graph_span_label(char *out, size_t out_size, int cells_w, double seconds_per_sample);

// Draws `n` samples (oldest first) into the graph whose top-left corner is
// (x, y). The newest sample is the rightmost dot column; with fewer samples
// than columns the left part stays empty, as btop's graph fills in from the
// right. A sample of `scale` or more fills a whole column; any sample above
// zero lights at least one dot, so small amounts never vanish entirely.
//
// `grow_up` true: columns rise from the bottom edge (the bottom dot row is
// level 0). false: they hang down from the top edge. `gradient` is the
// colour at the lowest level, the middle and the top level, interpolated
// linearly in between (btop's own green -> yellow -> red idea).
void braille_graph_draw(cairo_t *cr, double x, double y, int cells_w, int cells_h,
                        const double *samples, int n, double scale, bool grow_up,
                        const Color gradient[3]);

// --- Sprite path, for graphs redrawn every frame (the media dropdown's
// spectrum, 15fps) ---
//
// braille_graph_draw() fills one antialiased circle per lit dot -- fine for
// the network graph's once-per-5s repaint, but up to thousands per frame for
// an animated one. Instead, render every dot once, lit, into an image, and
// per frame copy just the lit part of each column out of it: whole-pixel
// rectangles (dot centres sit on half pixels with a 3px pitch, so a column is
// exactly the 3px band around its centre). Same geometry and colours as
// braille_graph_draw(), so the two look identical. The copying itself is the
// caller's (modules/media/media_popup.c does it with memcpy: even batched
// into one path per fill, cairo's per-call overhead cost more than the few
// pixels moved).

// A `braille_graph_width(cells_w)` x `braille_graph_height(cells_h)` image
// with every dot lit, coloured by row like braille_graph_draw(); `grow_up`
// picks which edge is level 0. Caller destroys it.
cairo_surface_t *braille_graph_render_lit(int cells_w, int cells_h, bool grow_up, const Color gradient[3]);

// Where dot rows [row_from, row_to) of column `col` are, graph-local whole
// pixels (row_to > row_from): copying exactly this rectangle out of a
// braille_graph_render_lit() image -- or out of a panel image with one
// composited onto it -- lights those dots and nothing else.
void braille_graph_column_rect(int cells_h, int col, int row_from, int row_to, bool grow_up, int *x, int *y,
                               int *w, int *h);

// Dot rows per column for a graph `cells_h` cells tall.
int braille_graph_rows(int cells_h);

#endif
