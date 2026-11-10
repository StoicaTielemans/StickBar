#include "modules/network/network.h"

#include <linux/input-event-codes.h> // BTN_RIGHT
#include <stdio.h>

#include "config/config.h"
#include "config/glyphs.h"
#include "modules/network/network_info_popup.h"
#include "network/network_client.h"
#include "render/box_rects.h"
#include "render/module_box.h"

// Where this box was last drawn, per output (render/box_rects.h).
static BoxRectTable g_rects;

// Matches NetworkWidget.qml's icon() exactly: no default route at all, then
// disconnected/connected each split on Wi-Fi vs. Ethernet.
uint32_t network_status_glyph(void) {
    if (network_interface_name()[0] == '\0') {
        return GLYPH_ICON_NETWORK_UNKNOWN;
    }
    if (!network_connected()) {
        return network_is_wifi() ? GLYPH_ICON_WIFI_OFF : GLYPH_ICON_ETHERNET_OFF;
    }
    return network_is_wifi() ? GLYPH_ICON_WIFI_ON : GLYPH_ICON_ETHERNET_ON;
}

// Matches NetworkMonitor.qml's own formatRate() thresholds/precision exactly.
void network_format_rate(double bytes_per_sec, char *out, size_t out_size) {
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
    glyph_utf8(network_status_glyph(), icon_buf);
    glyph_utf8(GLYPH_ARROW_DOWN, down_buf);
    glyph_utf8(GLYPH_ARROW_UP, up_buf);

    char label[96];
    if (network_connected()) {
        char rx_text[32], tx_text[32];
        network_format_rate(network_rx_rate_bytes_per_sec(), rx_text, sizeof(rx_text));
        network_format_rate(network_tx_rate_bytes_per_sec(), tx_text, sizeof(tx_text));
        snprintf(label, sizeof(label), "%s %s%s %s%s", icon_buf, down_buf, rx_text, up_buf, tx_text);
    } else {
        snprintf(label, sizeof(label), "%s Disconnected", icon_buf);
    }

    return module_box_label(cr, &g_rects, output, MODULE_BOX_FROM_RIGHT, right_edge_x, box_top, box_height, label,
                            COLORS.teal);
}

void network_handle_click(Output *output, double x, double y, uint32_t button) {
    if (button != BTN_RIGHT) {
        return;
    }
    const BoxRect *rect = box_rect_hit(&g_rects, output, x, y);
    if (!rect) {
        return;
    }

    network_info_popup_open(output, rect->x, rect->y, rect->w, rect->h);
}
