#ifndef CUSTOMBAR_RENDER_RENDER_H
#define CUSTOMBAR_RENDER_RENDER_H

#include <cairo.h>

#include "wayland/context.h"

// Draws one full frame for `output` into `cr`, which is already backed by a
// `width`x`height` pixel buffer (the bar surface's current size). Fills the
// background, then dispatches to each bar module in its region: left (workspaces),
// right (clock). The center region is intentionally left empty -- scaffolding for
// modules added in later passes.
void render_output(cairo_t *cr, Output *output, int width, int height);

#endif
