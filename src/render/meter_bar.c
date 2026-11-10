#include "render/meter_bar.h"

#include <math.h>

#include "render/color.h"

#define SEGMENT_GAP 1.0

void meter_bar_draw(cairo_t *cr, double x, double y, double w, double h, double fraction, int segments,
                    const Color gradient[3], Color empty) {
    if (segments <= 0 || w <= 0.0) {
        return;
    }
    if (fraction < 0.0) {
        fraction = 0.0;
    }
    if (fraction > 1.0) {
        fraction = 1.0;
    }
    int lit = (int)lround(fraction * segments);
    // Anything above zero shows at least one block, so "barely used" doesn't
    // look identical to "nothing" (same rule as braille_graph's one-dot minimum).
    if (lit == 0 && fraction > 0.0) {
        lit = 1;
    }

    double seg_w = (w - SEGMENT_GAP * (segments - 1)) / segments;
    cairo_save(cr);
    for (int i = 0; i < segments; i++) {
        Color c = empty;
        if (i < lit) {
            double t = segments > 1 ? (double)i / (segments - 1) : 0.0;
            c = color_gradient3(gradient, t);
        }
        color_set_source(cr, c);
        cairo_rectangle(cr, x + i * (seg_w + SEGMENT_GAP), y, seg_w, h);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}
