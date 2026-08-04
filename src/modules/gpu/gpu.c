#include "modules/gpu/gpu.h"

#include <linux/input-event-codes.h> // BTN_RIGHT
#include <stdio.h>

#include "config/config.h"
#include "config/glyphs.h"
#include "render/module_box.h"
#include "sysmon/sysmon_client.h"
#include "sysmon/sysmon_info_popup.h"

// How many different Output pointers we keep the box's hit-rect for -- sized
// generously (this machine has 2 monitors), matching the convention already
// used by every other per-output hit-rect table in this project.
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

double gpu_render(cairo_t *cr, Output *output, double x, double box_top, double box_height) {
    char icon_buf[5];
    glyph_utf8(GLYPH_ICON_GPU, icon_buf);

    char label[32];
    if (sysmon_gpu_available()) {
        snprintf(label, sizeof(label), "%s %d%% %d°C", icon_buf, sysmon_gpu_percent(), sysmon_gpu_temp_c());
    } else {
        snprintf(label, sizeof(label), "%s --", icon_buf);
    }

    double text_w, text_h;
    module_box_measure_text(label, FONT.label, true, &text_w, &text_h);
    (void)text_h;

    double box_w = text_w + 12.0;

    module_box_draw_rect(cr, x, box_top, box_w, box_height,
                          COLORS.mantle, COLORS.green, (double)BAR.module_border_width);
    module_box_draw_text_centered(cr, x, box_top, box_w, box_height, label, FONT.label, true, COLORS.green);

    OutputBoxRect *rect = rect_for_output(output);
    if (rect) {
        rect->x = x;
        rect->y = box_top;
        rect->w = box_w;
        rect->h = box_height;
    }

    return x + box_w + BAR.item_spacing;
}

void gpu_handle_click(Output *output, double x, double y, uint32_t button) {
    if (button != BTN_RIGHT) {
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

    char body[512];
    if (sysmon_gpu_available()) {
        // Mirrors the reference SystemMonitor.qml's buildTooltip()'s GPU section.
        snprintf(body, sizeof(body),
                 "UTIL: %d%% | MEM: %d%%\n"
                 "TEMP: %d°C\n"
                 "VRAM: %d/%d MiB\n"
                 "POWER: %.2fW | FAN: %d%%\n"
                 "PSTATE: %s",
                 sysmon_gpu_percent(), sysmon_gpu_mem_percent(), sysmon_gpu_temp_c(),
                 sysmon_gpu_mem_used_mb(), sysmon_gpu_mem_total_mb(), sysmon_gpu_power_w(),
                 sysmon_gpu_fan_percent(), sysmon_gpu_pstate());
    } else {
        snprintf(body, sizeof(body), "No NVIDIA GPU detected\n(nvidia-smi unavailable or failed)");
    }

    sysmon_info_popup_open(output, COLORS.green, body, rect->x, rect->y, rect->w, rect->h);
}
