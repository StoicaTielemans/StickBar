#include "modules/powermenu/arch_logo.h"

#include <linux/input-event-codes.h> // BTN_LEFT

#include "config/config.h"
#include "config/glyphs.h"
#include "modules/powermenu/power_menu.h"
#include "render/module_box.h"

// How many different Output pointers we keep the box's hit-rect for -- sized
// generously (this machine has 2 monitors), matching every other per-output
// hit-rect table in this project.
#define MAX_TRACKED_OUTPUTS 8

typedef struct {
    Output *output; // NULL if this slot is unused
    double x, y, w, h;
} OutputBoxRect;

static OutputBoxRect g_rects[MAX_TRACKED_OUTPUTS];

static OutputBoxRect *rect_for_output(Output *output) {
    OutputBoxRect *free_slot = NULL;
    for (int i = 0; i < MAX_TRACKED_OUTPUTS; i++) {
        if (g_rects[i].output == output) {
            return &g_rects[i];
        }
        if (!free_slot && g_rects[i].output == NULL) {
            free_slot = &g_rects[i];
        }
    }
    if (free_slot) {
        free_slot->output = output;
    }
    return free_slot;
}

double arch_logo_render(cairo_t *cr, Output *output, double x, double box_top, double box_height) {
    char glyph_buf[5];
    glyph_utf8(GLYPH_ICON_ARCH_LOGO, glyph_buf);

    // Sized from the glyph's *ink* extent, not its logical/advance extent
    // (module_box_measure_text() would give 8px; the glyph's actual ink is
    // 14px at this size -- see module_box_measure_text_ink()'s own doc
    // comment for the full story). Using the standard +12px module pad on
    // top of that (not the reference's own tighter +8, which was measured
    // against the *logical* width and left part of the glyph's ink landing
    // outside the box -- a real, confirmed bug, not a hypothetical) leaves
    // comfortable margin on both sides once module_box_draw_text_centered()
    // does its own (logical-width-based) centering.
    double ink_w, ink_h;
    module_box_measure_text_ink(glyph_buf, FONT.label, true, &ink_w, &ink_h);
    (void)ink_h;
    double box_w = ink_w + 12.0;

    // No border (border_width 0.0) -- the "cutout" look, unlike every other
    // module box's bordered chrome.
    module_box_draw_rect(cr, x, box_top, box_w, box_height, COLORS.sapphire, (Color){ 0, 0, 0, 0 }, 0.0);
    module_box_draw_text_centered(cr, x, box_top, box_w, box_height, glyph_buf, FONT.label, true, COLORS.mantle);

    OutputBoxRect *rect = rect_for_output(output);
    if (rect) {
        rect->x = x;
        rect->y = box_top;
        rect->w = box_w;
        rect->h = box_height;
    }

    return x + box_w + BAR.item_spacing;
}

void arch_logo_handle_click(Output *output, double x, double y, uint32_t button) {
    if (button != BTN_LEFT) {
        return;
    }
    OutputBoxRect *rect = NULL;
    for (int i = 0; i < MAX_TRACKED_OUTPUTS; i++) {
        if (g_rects[i].output == output) {
            rect = &g_rects[i];
            break;
        }
    }
    if (!rect || x < rect->x || x >= rect->x + rect->w || y < rect->y || y >= rect->y + rect->h) {
        return;
    }

    power_menu_toggle();
}
