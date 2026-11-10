#include "modules/volume/volume.h"

#include <linux/input-event-codes.h> // BTN_LEFT, BTN_RIGHT, BTN_MIDDLE
#include <stdio.h>

#include "audio/device_popup.h"
#include "audio/mixer_popup.h"
#include "audio/pulse_client.h"
#include "audio/volume_style.h"
#include "config/config.h"
#include "config/glyphs.h"
#include "render/box_rects.h"
#include "render/module_box.h"

// Where this box was last drawn, per output (render/box_rects.h).
static BoxRectTable g_rects;

double volume_render(cairo_t *cr, Output *output, double right_edge_x, double box_top, double box_height) {
    int percent = pulse_default_sink_volume_percent();
    bool muted = pulse_default_sink_muted();

    char icon_buf[5];
    glyph_utf8(volume_level_glyph(percent, muted), icon_buf);

    char label[32];
    snprintf(label, sizeof(label), "%s %d%%", icon_buf, percent);

    return module_box_label(cr, &g_rects, output, MODULE_BOX_FROM_RIGHT, right_edge_x, box_top, box_height, label,
                            COLORS.sky);
}

void volume_handle_click(Output *output, double x, double y, uint32_t button) {
    const BoxRect *rect = box_rect_hit(&g_rects, output, x, y);
    if (!rect) {
        return;
    }

    if (button == BTN_LEFT) {
        pulse_default_sink_toggle_mute();
    } else if (button == BTN_RIGHT) {
        // The device dropdown and the mixer both hang off this one box at the
        // same spot -- close the other so they never stack on top of each other.
        mixer_popup_close();
        device_popup_open(output, true, COLORS.sky, rect->x, rect->y, rect->w, rect->h);
    } else if (button == BTN_MIDDLE) {
        device_popup_close();
        mixer_popup_open(output, COLORS.sky, rect->x, rect->y, rect->w, rect->h);
    }
}

void volume_handle_scroll(Output *output, double x, double y, int direction) {
    const BoxRect *rect = box_rect_hit(&g_rects, output, x, y);
    if (!rect) {
        return;
    }

    pulse_default_sink_adjust_volume(direction > 0 ? PULSE_VOLUME_STEP_PERCENT : -PULSE_VOLUME_STEP_PERCENT);
}
