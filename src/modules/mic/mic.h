#ifndef CUSTOMBAR_MODULES_MIC_MIC_H
#define CUSTOMBAR_MODULES_MIC_MIC_H

#include <cairo.h>
#include <stdint.h>

#include "wayland/context.h"

// Draws the default source's (microphone) module box right-anchored so its
// right edge sits at `right_edge_x`, vertically spanning
// box_top..box_top+box_height. Records the box's screen-space rect for later
// mic_handle_click()/mic_handle_scroll(). Returns the box's own left edge, for
// the next (further left) right-region module to anchor against.
double mic_render(cairo_t *cr, Output *output, double right_edge_x, double box_top, double box_height);

// Hit-tests (x, y) against `output`'s most recently rendered box. Left-click
// toggles mute; right-click opens the source device dropdown
// (audio/device_popup.c). No-op if (x, y) doesn't land on the box.
void mic_handle_click(Output *output, double x, double y, uint32_t button);

// Hit-tests (x, y) against `output`'s most recently rendered box; if it lands
// on it, adjusts the default source's volume by one
// PULSE_VOLUME_STEP_PERCENT step in `direction`'s sign (magnitude ignored --
// one call represents one wheel notch). No-op otherwise.
void mic_handle_scroll(Output *output, double x, double y, int direction);

#endif
