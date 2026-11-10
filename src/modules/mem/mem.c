#include "modules/mem/mem.h"

#include <linux/input-event-codes.h> // BTN_RIGHT
#include <stdio.h>

#include "config/config.h"
#include "config/glyphs.h"
#include "modules/mem/mem_popup.h"
#include "render/box_rects.h"
#include "render/module_box.h"
#include "sysmon/sysmon_client.h"

// Where this box was last drawn, per output (render/box_rects.h).
static BoxRectTable g_rects;

double mem_render(cairo_t *cr, Output *output, double x, double box_top, double box_height) {
    char icon_buf[5];
    glyph_utf8(GLYPH_ICON_MEM, icon_buf);

    char label[32];
    snprintf(label, sizeof(label), "%s %d%%", icon_buf, sysmon_mem_percent());

    return module_box_label(cr, &g_rects, output, MODULE_BOX_FROM_LEFT, x, box_top, box_height, label, COLORS.yellow);
}

void mem_handle_click(Output *output, double x, double y, uint32_t button) {
    if (button != BTN_RIGHT) {
        return;
    }
    const BoxRect *rect = box_rect_hit(&g_rects, output, x, y);
    if (!rect) {
        return;
    }

    mem_popup_open(output, rect->x, rect->y, rect->w, rect->h);
}
