#ifndef CUSTOMBAR_RENDER_RENDER_H
#define CUSTOMBAR_RENDER_RENDER_H

#include <cairo.h>
#include <stdbool.h>

#include "wayland/context.h"

// Draws one full frame for `output` into `cr`, which is already backed by a
// `width`x`height` pixel buffer (the bar surface's current size). Fills the
// background, then dispatches to each bar module in its region: left (workspaces),
// right (clock). The center region is intentionally left empty -- scaffolding for
// modules added in later passes.
void render_output(cairo_t *cr, Output *output, int width, int height);

// Repaints only the clock box, assuming `cr`'s buffer already holds the
// previous full frame (bar/bar_surface.c copies it in first). Returns false --
// and leaves the caller to do a full render_output() instead -- if the clock
// hasn't been drawn on this output yet, or if its box came out a different
// width this time (anything around it would then need to move too).
bool render_output_clock_only(cairo_t *cr, Output *output, int width, int height);

#endif
