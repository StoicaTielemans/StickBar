#include "modules/mic/mic.h"

#include <linux/input-event-codes.h> // BTN_LEFT, BTN_RIGHT
#include <stdio.h>

#include "audio/device_popup.h"
#include "audio/pulse_client.h"
#include "config/config.h"
#include "config/glyphs.h"
#include "render/module_box.h"

// How many different Output pointers we keep the box's hit-rect for -- sized
// generously (this machine has 2 monitors), matching modules/tray/tray.c's
// own MAX_TRACKED_OUTPUTS convention.
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

double mic_render(cairo_t *cr, Output *output, double right_edge_x, double box_top, double box_height) {
    int percent = pulse_default_source_volume_percent();
    bool muted = pulse_default_source_muted();

    char icon_buf[5];
    // Matches MicWidget.qml's icon(): a simple 2-state glyph, unlike the
    // sink's 4-level one.
    glyph_utf8(muted || percent == 0 ? GLYPH_ICON_MIC_MUTED : GLYPH_ICON_MIC, icon_buf);

    char label[32];
    snprintf(label, sizeof(label), "%s %d%%", icon_buf, percent);

    double text_w, text_h;
    module_box_measure_text(label, FONT.label, true, &text_w, &text_h);
    (void)text_h;

    // Matches the reference MicWidget.qml's `implicitWidth: label.implicitWidth + 12`.
    double box_w = text_w + 12.0;
    double box_x = right_edge_x - box_w;

    module_box_draw_rect(cr, box_x, box_top, box_w, box_height,
                          COLORS.mantle, COLORS.blue, (double)BAR.module_border_width);
    module_box_draw_text_centered(cr, box_x, box_top, box_w, box_height, label, FONT.label, true, COLORS.blue);

    OutputBoxRect *rect = rect_for_output(output);
    if (rect) {
        rect->x = box_x;
        rect->y = box_top;
        rect->w = box_w;
        rect->h = box_height;
    }

    // Matches tray_render()'s/clock_render()'s convention of baking
    // BAR.item_spacing into the returned x, so the next (further left)
    // right-region module renders with a gap rather than flush against this box.
    return box_x - BAR.item_spacing;
}

void mic_handle_click(Output *output, double x, double y, uint32_t button) {
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

    if (button == BTN_LEFT) {
        pulse_default_source_toggle_mute();
    } else if (button == BTN_RIGHT) {
        device_popup_open(output, false, COLORS.blue, rect->x, rect->y, rect->w, rect->h);
    }
}

void mic_handle_scroll(Output *output, double x, double y, int direction) {
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

    pulse_default_source_adjust_volume(direction > 0 ? PULSE_VOLUME_STEP_PERCENT : -PULSE_VOLUME_STEP_PERCENT);
}
