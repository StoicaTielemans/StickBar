#ifndef CUSTOMBAR_MODULES_TRAY_TRAY_H
#define CUSTOMBAR_MODULES_TRAY_TRAY_H

#include <cairo.h>
#include <stdint.h>

#include "wayland/context.h"

// Draws the tray's row of icons flush against the right edge (`right_edge_x`),
// vertically centered in a box of height `box_height`, in registration order
// left-to-right within the row. Draws nothing and returns `right_edge_x`
// unchanged when there are no tracked items, matching the reference Tray.qml's
// `visible: trayRepeater.count > 0` (removes it from layout space entirely, not
// just hides it). Records each icon's screen-space rect (and which item it
// belongs to) for later tray_handle_click(). Returns the x coordinate the next
// (further left) right-region widget should end at.
double tray_render(cairo_t *cr, Output *output, double right_edge_x, double box_top, double box_height);

// Hit-tests (x, y) against `output`'s most recently rendered icon rects; if it
// lands on one, decides Activate() vs opening the context menu per the
// has-menu-overrides-button rule documented in tray.c, and dispatches
// accordingly. No-op if it doesn't land on any icon.
void tray_handle_click(Output *output, double x, double y, uint32_t button);

#endif
