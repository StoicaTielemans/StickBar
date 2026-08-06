#include "modules/network/network.h"

#include <linux/input-event-codes.h> // BTN_RIGHT
#include <stdio.h>

#include "config/config.h"
#include "config/glyphs.h"
#include "modules/network/network_info_popup.h"
#include "network/network_client.h"
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

// Matches NetworkWidget.qml's icon() exactly: no default route at all, then
// disconnected/connected each split on Wi-Fi vs. Ethernet.
static uint32_t status_icon_glyph(void) {
    if (network_interface_name()[0] == '\0') {
        return GLYPH_ICON_NETWORK_UNKNOWN;
    }
    if (!network_connected()) {
        return network_is_wifi() ? GLYPH_ICON_WIFI_OFF : GLYPH_ICON_ETHERNET_OFF;
    }
    return network_is_wifi() ? GLYPH_ICON_WIFI_ON : GLYPH_ICON_ETHERNET_ON;
}

// Matches NetworkMonitor.qml's own formatRate() thresholds/precision exactly.
static void format_rate(double bytes_per_sec, char *out, size_t out_size) {
    if (bytes_per_sec >= 1024.0 * 1024.0) {
        snprintf(out, out_size, "%.1f MB/s", bytes_per_sec / 1024.0 / 1024.0);
    } else if (bytes_per_sec >= 1024.0) {
        snprintf(out, out_size, "%.1f KB/s", bytes_per_sec / 1024.0);
    } else {
        snprintf(out, out_size, "%.0f B/s", bytes_per_sec);
    }
}

double network_render(cairo_t *cr, Output *output, double right_edge_x, double box_top, double box_height) {
    char icon_buf[5], down_buf[5], up_buf[5];
    glyph_utf8(status_icon_glyph(), icon_buf);
    glyph_utf8(GLYPH_ARROW_DOWN, down_buf);
    glyph_utf8(GLYPH_ARROW_UP, up_buf);

    char label[96];
    if (network_connected()) {
        char rx_text[32], tx_text[32];
        format_rate(network_rx_rate_bytes_per_sec(), rx_text, sizeof(rx_text));
        format_rate(network_tx_rate_bytes_per_sec(), tx_text, sizeof(tx_text));
        snprintf(label, sizeof(label), "%s %s%s %s%s", icon_buf, down_buf, rx_text, up_buf, tx_text);
    } else {
        snprintf(label, sizeof(label), "%s Disconnected", icon_buf);
    }

    double text_w, text_h;
    module_box_measure_text(label, FONT.label, true, &text_w, &text_h);
    (void)text_h;

    // Matches this project's other module boxes' `implicitWidth: label + 12`
    // convention.
    double box_w = text_w + 12.0;
    double box_x = right_edge_x - box_w;

    module_box_draw_rect(cr, box_x, box_top, box_w, box_height,
                          COLORS.mantle, COLORS.teal, (double)BAR.module_border_width);
    module_box_draw_text_centered(cr, box_x, box_top, box_w, box_height, label, FONT.label, true, COLORS.teal);

    OutputBoxRect *rect = rect_for_output(output);
    if (rect) {
        rect->x = box_x;
        rect->y = box_top;
        rect->w = box_w;
        rect->h = box_height;
    }

    return box_x - BAR.item_spacing;
}

void network_handle_click(Output *output, double x, double y, uint32_t button) {
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

    network_info_popup_open(output, rect->x, rect->y, rect->w, rect->h);
}
