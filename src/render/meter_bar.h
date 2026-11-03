#ifndef CUSTOMBAR_RENDER_METER_BAR_H
#define CUSTOMBAR_RENDER_METER_BAR_H

#include <cairo.h>

#include "config/config.h"

/*
 * A segmented "how full is it" bar (btop's ▰▰▰▱▱ meters): `segments` equal
 * blocks with a 1px gap, the first round(fraction * segments) of them lit.
 * Lit blocks take their colour from where they sit along the bar, through
 * `gradient` (low, middle, high) -- so a nearly full bar ends in the "hot"
 * colour while a low one stays in the calm one. Unlit blocks are `empty`.
 * Pure cairo, like render/braille_graph.c; first used by the GPU dropdown.
 */
void meter_bar_draw(cairo_t *cr, double x, double y, double w, double h, double fraction, int segments,
                    const Color gradient[3], Color empty);

#endif
