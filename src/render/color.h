#ifndef CUSTOMBAR_RENDER_COLOR_H
#define CUSTOMBAR_RENDER_COLOR_H

#include <cairo.h>

#include "config/config.h"

// Small Color helpers shared by every renderer -- each of these used to be
// written out (or reimplemented) locally in several popups and render files.

// cairo_set_source_rgba() from a config.h Color.
static inline void color_set_source(cairo_t *cr, Color c) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}

// The same colour with its alpha scaled by `factor` (0..1).
static inline Color color_with_alpha(Color c, double factor) {
    c.a *= factor;
    return c;
}

static inline Color color_mix(Color a, Color b, double f) {
    Color c = {a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f, a.a + (b.a - a.a) * f};
    return c;
}

// A 3-stop gradient (low, middle, high) sampled at t in 0..1, interpolated
// linearly within each half -- btop's green -> yellow -> red idea, used by the
// meters (render/meter_bar.c) and braille graphs (render/braille_graph.c).
static inline Color color_gradient3(const Color g[3], double t) {
    return t < 0.5 ? color_mix(g[0], g[1], t * 2.0) : color_mix(g[1], g[2], (t - 0.5) * 2.0);
}

#endif
