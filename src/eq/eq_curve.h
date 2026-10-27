#ifndef CUSTOMBAR_EQ_EQ_CURVE_H
#define CUSTOMBAR_EQ_EQ_CURVE_H

#include <cairo.h>
#include <stdbool.h>

#include "config/config.h"

/*
 * Pure cairo drawing of eq/eq_state.c's current EQ curve -- no inotify, file,
 * or process handling in this file, deliberately (same split as
 * dbus/sni_icon.c / mpris/mpris_art.c: plumbing in one file, pure rendering in
 * another). Shared by the bar box (modules/eq/eq.c, a ~14px-tall sparkline)
 * and the dropdown (modules/eq/eq_popup.c, a full graph), which differ only
 * in size, scale and whether a grid is drawn.
 */

// The dB span shown above/below 0 dB: the smallest multiple of `step` that
// contains the curve's own peak/min, but never less than `min_range`. Keeps a
// gentle EQ (e.g. a few treble cuts) from being stretched into a dramatic-
// looking squiggle, while still fitting a heavy one (e.g. +19 dB of bass).
double eq_curve_range_db(double min_range, double step);

// Maps a frequency (Hz) to an x position within [x, x + w], log-scaled over
// 20 Hz..20 kHz -- exposed so the popup can place its frequency grid lines on
// exactly the same scale the curve itself is drawn with.
double eq_curve_freq_to_x(double freq, double x, double w);

// Strokes the current curve into the (x, y, w, h) rect, with 0 dB at the
// rect's vertical middle and +/-range_db at its top/bottom edges. Draws a
// faint 0 dB reference line first if `zero_line` is set. No-op if no curve
// has been loaded.
void eq_curve_draw(cairo_t *cr, double x, double y, double w, double h, double range_db,
                   Color color, double line_width, bool zero_line, Color zero_color);

#endif
