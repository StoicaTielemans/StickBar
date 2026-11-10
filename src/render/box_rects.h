#ifndef CUSTOMBAR_RENDER_BOX_RECTS_H
#define CUSTOMBAR_RENDER_BOX_RECTS_H

#include <stdbool.h>

#include "wayland/context.h"

// Where a bar module's box was last drawn, per output -- what its click
// handler hit-tests against and what its dropdown anchors under. Every bar
// module used to declare its own identical table and lookup for this.

// Sized generously (this machine has 2 monitors).
#define BOX_RECT_MAX_OUTPUTS 8

typedef struct {
    double x, y, w, h;
} BoxRect;

typedef struct {
    Output *output[BOX_RECT_MAX_OUTPUTS]; // NULL = unused slot
    BoxRect rect[BOX_RECT_MAX_OUTPUTS];
} BoxRectTable;

// Records the box drawn on `output`. A zero-size rect means "not drawn here
// right now" (a collapsed module) -- it never hit-tests, so a stale click
// can't land on a box that's no longer shown.
void box_rect_set(BoxRectTable *table, Output *output, double x, double y, double w, double h);
void box_rect_clear(BoxRectTable *table, Output *output);

// The box on `output` if (x, y) lands inside it, else NULL.
const BoxRect *box_rect_hit(const BoxRectTable *table, const Output *output, double x, double y);

// The box on `output`, if one is currently drawn there (false for none, or
// zero-size).
bool box_rect_get(const BoxRectTable *table, const Output *output, BoxRect *out);

#endif
