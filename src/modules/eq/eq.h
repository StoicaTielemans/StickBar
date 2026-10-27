#ifndef CUSTOMBAR_MODULES_EQ_EQ_H
#define CUSTOMBAR_MODULES_EQ_EQ_H

#include <cairo.h>
#include <stdint.h>

#include "wayland/context.h"

// Draws the DAC EQ module box (a mini curve sparkline + the active profile's
// name) right-anchored so its right edge sits at `right_edge_x` -- same shape
// as modules/volume/volume.c's volume_render(), since this lives in the bar's
// right region just left of Volume. Records the box's screen-space rect for
// later eq_handle_click(). Returns the box's own left edge minus
// BAR.item_spacing, for the next (further left) right-region module.
double eq_render(cairo_t *cr, Output *output, double right_edge_x, double box_top, double box_height);

// Hit-tests (x, y) against `output`'s most recently rendered box. Both left-
// and right-click open the profile dropdown (modules/eq/eq_popup.c): unlike
// volume/mic, this widget has no separate "quick" primary action worth
// binding to left-click -- choosing a profile *is* its primary action.
// No-op if (x, y) doesn't land on the box.
void eq_handle_click(Output *output, double x, double y, uint32_t button);

#endif
