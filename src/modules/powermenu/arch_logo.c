#include "modules/powermenu/arch_logo.h"

#include <linux/input-event-codes.h> // BTN_LEFT

#include "config/config.h"
#include "config/glyphs.h"
#include "modules/powermenu/power_menu.h"
#include "render/box_rects.h"
#include "render/module_box.h"

// Where this box was last drawn, per output (render/box_rects.h).
static BoxRectTable g_rects;

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

    box_rect_set(&g_rects, output, x, box_top, box_w, box_height);

    return x + box_w + BAR.item_spacing;
}

void arch_logo_handle_click(Output *output, double x, double y, uint32_t button) {
    if (button != BTN_LEFT) {
        return;
    }
    const BoxRect *rect = box_rect_hit(&g_rects, output, x, y);
    if (!rect) {
        return;
    }

    power_menu_toggle();
}
