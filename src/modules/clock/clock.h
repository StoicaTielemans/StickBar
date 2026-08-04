#ifndef CUSTOMBAR_MODULES_CLOCK_CLOCK_H
#define CUSTOMBAR_MODULES_CLOCK_CLOCK_H

#include <cairo.h>

// Recomputes the cached formatted time string from the current wall clock. Call
// once at startup and then once per second thereafter (driven by main.c's
// timerfd) -- clock_render() just reads the cached string rather than
// reformatting it on every frame.
void clock_tick(void);

// Draws the clock's module box right-anchored so its right edge sits at
// `right_edge_x`, vertically spanning box_top..box_top+box_height. Returns the
// box's own left edge minus BAR.item_spacing, so the next (further left)
// right-region module renders with a gap rather than flush against this box
// (mirroring tray_render()'s identical convention, and workspaces_render()'s
// equivalent for the left region).
double clock_render(cairo_t *cr, double right_edge_x, double box_top, double box_height);

#endif
