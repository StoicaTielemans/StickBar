#ifndef CUSTOMBAR_RENDER_RENDER_H
#define CUSTOMBAR_RENDER_RENDER_H

#include <cairo.h>
#include <stdbool.h>

#include "wayland/context.h"

// Draws one full frame for `output` into `cr`, which is already backed by a
// `width`x`height` pixel buffer (the bar surface's current size). Fills the
// background, then draws each region's modules (render.c's LEFT_REGION and
// RIGHT_REGION tables), then the media box centered between them.
void render_output(cairo_t *cr, Output *output, int width, int height);

// Repaints only the clock box, assuming `cr`'s buffer already holds the
// previous full frame (bar/bar_surface.c copies it in first). Returns false --
// and leaves the caller to do a full render_output() instead -- if the clock
// hasn't been drawn on this output yet, or if its box came out a different
// width this time (anything around it would then need to move too).
bool render_output_clock_only(cairo_t *cr, Output *output, int width, int height);

// Where each module's box was drawn in `output`'s last full frame, by module
// name ("arch", "workspaces", "cpu", "gpu", "mem", "privacy", "tray",
// "clock", "network", "eq", "volume", "mic", "media"). False if that module
// hasn't been drawn there or is collapsed (zero width). Recorded on every
// full render -- a few stores per frame -- and read only by the test-control
// channel (testing/test_control.c), which uses it to find where to click.
bool render_module_rect(Output *output, const char *module, double *x, double *y, double *w, double *h);

#endif
