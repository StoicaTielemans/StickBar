#ifndef CUSTOMBAR_RENDER_STAT_CARD_H
#define CUSTOMBAR_RENDER_STAT_CARD_H

#include <cairo.h>
#include <stddef.h>

#include "config/config.h"

// Pieces shared by the btop-style stat dropdowns (CPU, GPU, memory, network):
// the history graph card, its time axis, label/value rows and the top-apps
// list. Each of those dropdowns used to carry an identical copy of these.

#define STAT_CARD_PAD 8.0
#define STAT_GRAPH_LABEL_H 14.0    // the "usage 12%   peak 40%" row above a graph
#define STAT_GRAPH_LABEL_GAP 3.0   // label row -> graph
#define STAT_GRAPH_SPLIT_GAP 8.0   // between two graphs in one card
#define STAT_GRAPH_MIN_CELLS_W 30  // 5 minutes of 5s samples, one per dot column
#define STAT_AXIS_H 12.0           // the "8 min ago ... now" row under the card
#define STAT_ROW_H 17.0
#define STAT_APP_BAR_W 70.0
#define STAT_APP_BAR_H 5.0

// How many braille cells the graph in a `card_w`-wide card gets: as many as
// fit inside the card's padding (one 5s sample per dot column), capped at the
// whole history (`history_len` samples, 2 per cell). STAT_GRAPH_MIN_CELLS_W
// only sets a card's minimum width; a wider card shows more history.
int stat_graph_cells(double card_w, int history_len);

// The first history index a `cells`-wide graph shows (only the samples it
// has room for -- and so only those count toward its "peak" label), and
// through *n how many samples that leaves.
int stat_history_window(int history_len, int cells, int *n);

// A graph's label row: `left` bold in `left_color`, `right` dim and
// right-aligned, across a `w`-wide row at (x, y).
void stat_graph_label_row(cairo_t *cr, double x, double y, double w, const char *left, Color left_color,
                          const char *right);

// The time axis under a graph card: "<span> ago" at the left, "now" at the
// right, for a `cells`-wide graph of 5s samples. Returns the height it takes
// (draws nothing when cr is NULL, for measuring passes).
double stat_graph_axis(cairo_t *cr, double x, double y, double w, int cells);

// Width of the label column for rows labelled with `labels` (FONT.xs bold).
double stat_label_col_w(const char *const *labels, size_t count);

// One label/value row: the label dim and bold at x, the value at value_x.
void stat_draw_kv_row(cairo_t *cr, double x, double value_x, double y, const char *label, const char *value);

// The top-apps list's name column: /proc/PID/comm names are at most 15
// characters (TASK_COMM_LEN minus the NUL) and the font is monospace, so 15 of
// any glyph is exactly the widest a name can get.
double stat_app_name_col_w(void);

// One top-apps row: name, a 7-segment bar (`fraction` of the biggest app, in
// `bar_color`) and `value` right-aligned at x + w.
void stat_draw_app_row(cairo_t *cr, double x, double y, double w, const char *name, double fraction,
                       Color bar_color, const char *value);

#endif
