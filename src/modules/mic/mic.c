#include "modules/mic/mic.h"

#include <linux/input-event-codes.h> // BTN_LEFT, BTN_RIGHT
#include <stdio.h>

#include "audio/device_popup.h"
#include "audio/pulse_client.h"
#include "config/config.h"
#include "config/glyphs.h"
#include "render/box_rects.h"
#include "render/module_box.h"

// Where this box was last drawn, per output (render/box_rects.h).
static BoxRectTable g_rects;

double mic_render(cairo_t *cr, Output *output, double right_edge_x, double box_top, double box_height) {
    int percent = pulse_default_source_volume_percent();
    bool muted = pulse_default_source_muted();

    char icon_buf[5];
    // Matches MicWidget.qml's icon(): a simple 2-state glyph, unlike the
    // sink's 4-level one.
    glyph_utf8(muted || percent == 0 ? GLYPH_ICON_MIC_MUTED : GLYPH_ICON_MIC, icon_buf);

    char label[32];
    snprintf(label, sizeof(label), "%s %d%%", icon_buf, percent);

    return module_box_label(cr, &g_rects, output, MODULE_BOX_FROM_RIGHT, right_edge_x, box_top, box_height, label,
                            COLORS.blue);
}

void mic_handle_click(Output *output, double x, double y, uint32_t button) {
    const BoxRect *rect = box_rect_hit(&g_rects, output, x, y);
    if (!rect) {
        return;
    }

    if (button == BTN_LEFT) {
        pulse_default_source_toggle_mute();
    } else if (button == BTN_RIGHT) {
        device_popup_open(output, false, COLORS.blue, rect->x, rect->y, rect->w, rect->h);
    }
}

void mic_handle_scroll(Output *output, double x, double y, int direction) {
    const BoxRect *rect = box_rect_hit(&g_rects, output, x, y);
    if (!rect) {
        return;
    }

    pulse_default_source_adjust_volume(direction > 0 ? PULSE_VOLUME_STEP_PERCENT : -PULSE_VOLUME_STEP_PERCENT);
}
