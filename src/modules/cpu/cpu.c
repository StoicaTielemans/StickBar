#include "modules/cpu/cpu.h"

#include <linux/input-event-codes.h> // BTN_RIGHT
#include <math.h>
#include <stdio.h>

#include "config/config.h"
#include "config/glyphs.h"
#include "render/box_rects.h"
#include "render/module_box.h"
#include "sysmon/sysmon_client.h"
#include "modules/cpu/cpu_popup.h"

// Where this box was last drawn, per output (render/box_rects.h).
static BoxRectTable g_rects;

double cpu_render(cairo_t *cr, Output *output, double x, double box_top, double box_height) {
    char icon_buf[5];
    glyph_utf8(GLYPH_ICON_CPU, icon_buf);

    char label[32];
    snprintf(label, sizeof(label), "%s %d%% %d°C", icon_buf, (int)lround(sysmon_cpu_percent()),
              sysmon_cpu_temp_c());

    return module_box_label(cr, &g_rects, output, MODULE_BOX_FROM_LEFT, x, box_top, box_height, label, COLORS.peach);
}

void cpu_handle_click(Output *output, double x, double y, uint32_t button) {
    if (button != BTN_RIGHT) {
        return;
    }
    const BoxRect *rect = box_rect_hit(&g_rects, output, x, y);
    if (!rect) {
        return;
    }

    cpu_popup_open(output, rect->x, rect->y, rect->w, rect->h);
}
