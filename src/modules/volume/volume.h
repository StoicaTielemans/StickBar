#ifndef CUSTOMBAR_MODULES_VOLUME_VOLUME_H
#define CUSTOMBAR_MODULES_VOLUME_VOLUME_H

#include <cairo.h>
#include <stdint.h>

#include "wayland/context.h"

// Draws the default sink's (speaker/headset) module box right-anchored so its
// right edge sits at `right_edge_x`, vertically spanning
// box_top..box_top+box_height -- same shape as modules/clock/clock.c's
// clock_render(). Records the box's screen-space rect for later
// volume_handle_click()/volume_handle_scroll(). Returns the box's own left
// edge, for the next (further left) right-region module to anchor against.
double volume_render(cairo_t *cr, Output *output, double right_edge_x, double box_top, double box_height);

// Hit-tests (x, y) against `output`'s most recently rendered box. Left-click
// toggles mute; right-click opens the sink device dropdown (audio/device_popup.c).
// No-op if (x, y) doesn't land on the box.
void volume_handle_click(Output *output, double x, double y, uint32_t button);

// Hit-tests (x, y) against `output`'s most recently rendered box; if it lands
// on it, adjusts the default sink's volume by one PULSE_VOLUME_STEP_PERCENT
// step in `direction`'s sign (ignoring its magnitude -- one call is meant to
// represent exactly one wheel notch, already normalized by the caller in
// wayland/pointer.c). No-op otherwise.
void volume_handle_scroll(Output *output, double x, double y, int direction);

#endif
