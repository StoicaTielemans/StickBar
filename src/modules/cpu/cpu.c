#include "modules/cpu/cpu.h"

#include <linux/input-event-codes.h> // BTN_RIGHT
#include <math.h>
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

double cpu_render(cairo_t *cr, Output *output, double x, double box_top, double box_height) {
    char icon_buf[5];
    glyph_utf8(GLYPH_ICON_CPU, icon_buf);

    char label[32];
    snprintf(label, sizeof(label), "%s %d%% %d°C", icon_buf, (int)lround(sysmon_cpu_percent()),
              sysmon_cpu_temp_c());

    double text_w, text_h;
    module_box_measure_text(label, FONT.label, true, &text_w, &text_h);
    (void)text_h;

    // Matches this project's other module boxes' `implicitWidth: label + 12`
    // convention (clock/volume/mic's own boxes, mirroring the reference QML's
    // identical pattern).
    double box_w = text_w + 12.0;

    module_box_draw_rect(cr, x, box_top, box_w, box_height,
                          COLORS.mantle, COLORS.peach, (double)BAR.module_border_width);
    module_box_draw_text_centered(cr, x, box_top, box_w, box_height, label, FONT.label, true, COLORS.peach);

    OutputBoxRect *rect = rect_for_output(output);
    if (rect) {
        rect->x = x;
        rect->y = box_top;
        rect->w = box_w;
        rect->h = box_height;
    }

    return x + box_w + BAR.item_spacing;
}

void cpu_handle_click(Output *output, double x, double y, uint32_t button) {
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

    // Mirrors the reference SystemMonitor.qml's buildTooltip()'s CPU section
    // (frequency, temp, load average, then a 4-per-line core usage grid via
    // formatCoreLines()).
    char body[2048];
    int len = snprintf(body, sizeof(body),
                        "CPU: %.1f%% @ %.2fGHz (%d cores)\n"
                        "TEMP: %d°C\n"
                        "LOAD AVG: %s\n\n"
                        "CORES:\n",
                        sysmon_cpu_percent(), sysmon_cpu_freq_ghz(), sysmon_cpu_core_count(),
                        sysmon_cpu_temp_c(), sysmon_cpu_load_avg());

    for (int i = 0; i < sysmon_cpu_core_count() && len > 0 && (size_t)len < sizeof(body) - 32; i++) {
        int written = snprintf(body + len, sizeof(body) - (size_t)len, "%d: %.1f%%%s", i,
                                sysmon_cpu_core_percent(i), (i + 1) % 4 == 0 ? "\n" : "  ");
        len += written;
    }

    sysmon_info_popup_open(output, COLORS.peach, body, rect->x, rect->y, rect->w, rect->h);
}
