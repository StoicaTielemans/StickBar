#ifndef CUSTOMBAR_MODULES_CLOCK_CLOCK_H
#define CUSTOMBAR_MODULES_CLOCK_CLOCK_H

#include <cairo.h>
#include <stdint.h>

#include "wayland/context.h"

// Recomputes the cached formatted time string from the current wall clock. Call
// once at startup and then once per second thereafter (driven by main.c's
// timerfd) -- clock_render() just reads the cached string rather than
// reformatting it on every frame.
void clock_tick(void);

// Draws the clock's module box right-anchored so its right edge sits at
// `right_edge_x`, vertically spanning box_top..box_top+box_height. Records the
// box's screen-space rect for later clock_handle_click(). Returns the box's own
// left edge minus BAR.item_spacing, so the next (further left) right-region
// module renders with a gap rather than flush against this box (mirroring
// tray_render()'s identical convention, and workspaces_render()'s equivalent
// for the left region).
double clock_render(cairo_t *cr, Output *output, double right_edge_x, double box_top, double box_height);

// Hit-tests (x, y) against `output`'s most recently rendered box. Right-click
// toggles the year-calendar dropdown (modules/clock/clock_calendar_popup.c);
// left-click is a no-op, matching this project's established convention for
// modules with no obvious primary left-click action (see modules/cpu/cpu.h).
// No-op if (x, y) doesn't land on the box.
void clock_handle_click(Output *output, double x, double y, uint32_t button);

#endif
