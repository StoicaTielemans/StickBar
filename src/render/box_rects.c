#include "render/box_rects.h"

#include <stddef.h>

static int slot_of(const BoxRectTable *table, const Output *output) {
    for (int i = 0; i < BOX_RECT_MAX_OUTPUTS; i++) {
        if (table->output[i] == output) {
            return i;
        }
    }
    return -1;
}

void box_rect_set(BoxRectTable *table, Output *output, double x, double y, double w, double h) {
    int i = slot_of(table, output);
    if (i < 0) {
        i = slot_of(table, NULL); // claim a free slot
        if (i < 0) {
            return; // more outputs than slots: this one's box just isn't clickable
        }
        table->output[i] = output;
    }
    table->rect[i] = (BoxRect){x, y, w, h};
}

void box_rect_clear(BoxRectTable *table, Output *output) {
    int i = slot_of(table, output);
    if (i >= 0) {
        table->rect[i] = (BoxRect){0};
    }
}

const BoxRect *box_rect_hit(const BoxRectTable *table, const Output *output, double x, double y) {
    int i = output ? slot_of(table, output) : -1;
    if (i < 0) {
        return NULL;
    }
    const BoxRect *r = &table->rect[i];
    if (r->w <= 0 || x < r->x || x >= r->x + r->w || y < r->y || y >= r->y + r->h) {
        return NULL;
    }
    return r;
}

bool box_rect_get(const BoxRectTable *table, const Output *output, BoxRect *out) {
    int i = output ? slot_of(table, output) : -1;
    if (i < 0 || table->rect[i].w <= 0) {
        return false;
    }
    *out = table->rect[i];
    return true;
}
