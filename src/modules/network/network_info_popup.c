#include "modules/network/network_info_popup.h"

#include <stdio.h>

#include "bar/popup_surface.h"
#include "network/network_client.h"
#include "render/module_box.h"

// Sized generously (this machine has 2 monitors), matching the same
// MAX_TRACKED_OUTPUTS convention used by every other popup in this project.
#define MAX_TRACKED_OUTPUTS 8
#define OUTER_MARGIN 8.0 // matches Config.bar.popupPadding, same as every other popup
#define BODY_MAX_LEN 2048

typedef struct {
    bool open;
    Output *output;
    PopupSurface *popup;
    char body[BODY_MAX_LEN];
    double popup_width, popup_height;

    PopupSurface *catchers[MAX_TRACKED_OUTPUTS];
    struct wl_surface *catcher_surfaces[MAX_TRACKED_OUTPUTS];
    int catcher_count;
} InfoPopupState;

static InfoPopupState g_popup;

// Matches NetworkMonitor.qml's own formatBytes() thresholds/precision
// exactly (2 decimal places on GB/MB/KB) -- deliberately a different
// precision from format_rate() below (1 decimal place), same as the
// reference keeps formatRate()/formatBytes() as two distinct functions with
// two different precisions rather than one parameterized helper.
static void format_bytes(double bytes, char *out, size_t out_size) {
    if (bytes >= 1024.0 * 1024.0 * 1024.0) {
        snprintf(out, out_size, "%.2f GB", bytes / 1024.0 / 1024.0 / 1024.0);
    } else if (bytes >= 1024.0 * 1024.0) {
        snprintf(out, out_size, "%.2f MB", bytes / 1024.0 / 1024.0);
    } else if (bytes >= 1024.0) {
        snprintf(out, out_size, "%.2f KB", bytes / 1024.0);
    } else {
        snprintf(out, out_size, "%.0f B", bytes);
    }
}

// Matches NetworkMonitor.qml's own formatRate() thresholds/precision
// exactly -- see modules/network/network.c's own copy of this same
// algorithm for why it's duplicated locally rather than shared (this
// popup's body text needs the "B/s"-suffixed rate shape alongside the
// widget's own identical rate formatting, and the two files' own
// MAX_TRACKED_OUTPUTS-style local duplication is already this project's
// established convention).
static void format_rate(double bytes_per_sec, char *out, size_t out_size) {
    if (bytes_per_sec >= 1024.0 * 1024.0) {
        snprintf(out, out_size, "%.1f MB/s", bytes_per_sec / 1024.0 / 1024.0);
    } else if (bytes_per_sec >= 1024.0) {
        snprintf(out, out_size, "%.1f KB/s", bytes_per_sec / 1024.0);
    } else {
        snprintf(out, out_size, "%.0f B/s", bytes_per_sec);
    }
}

static const char *or_dash(const char *s) {
    return (s && s[0]) ? s : "-";
}

// Matches NetworkTooltip.qml's own NetworkMonitor.tooltip -> buildTooltip()
// text exactly, including which fields are omitted for Wi-Fi (link
// speed/duplex don't apply) and the blank-line section breaks.
static void build_body(char *out, size_t out_size) {
    const char *iface = network_interface_name();
    if (!iface[0]) {
        snprintf(out, out_size, "No active connection");
        return;
    }

    char rx_rate_s[32], tx_rate_s[32], rx_total[32], tx_total[32];
    format_rate(network_rx_rate_bytes_per_sec(), rx_rate_s, sizeof(rx_rate_s));
    format_rate(network_tx_rate_bytes_per_sec(), tx_rate_s, sizeof(tx_rate_s));
    format_bytes(network_rx_total_bytes(), rx_total, sizeof(rx_total));
    format_bytes(network_tx_total_bytes(), tx_total, sizeof(tx_total));

    bool is_wifi = network_is_wifi();
    int len = snprintf(out, out_size,
                        "INTERFACE: %s (%s)\n"
                        "STATUS: %s\n"
                        "IP ADDRESS: %s\n"
                        "GATEWAY: %s\n"
                        "MAC ADDRESS: %s\n"
                        "MTU: %s\n",
                        iface, is_wifi ? "Wi-Fi" : "Ethernet", network_connected() ? "Up" : "Down",
                        or_dash(network_ip_address()), or_dash(network_gateway()), or_dash(network_mac_address()),
                        or_dash(network_mtu()));

    if (!is_wifi && len > 0 && (size_t)len < out_size) {
        const char *speed = network_link_speed();
        char speed_text[32];
        if (speed[0]) {
            snprintf(speed_text, sizeof(speed_text), "%s Mbps", speed);
        } else {
            snprintf(speed_text, sizeof(speed_text), "-");
        }
        len += snprintf(out + len, out_size - (size_t)len, "LINK SPEED: %s (%s duplex)\n", speed_text,
                         or_dash(network_duplex()));
    }

    if (len > 0 && (size_t)len < out_size) {
        len += snprintf(out + len, out_size - (size_t)len, "\nDOWN: %s  |  UP: %s\nTOTAL RX: %s  |  TOTAL TX: %s\n",
                         rx_rate_s, tx_rate_s, rx_total, tx_total);
    }

    int dns_count = network_dns_count();
    if (dns_count > 0 && len > 0 && (size_t)len < out_size) {
        len += snprintf(out + len, out_size - (size_t)len, "\nDNS:\n");
        for (int i = 0; i < dns_count && len > 0 && (size_t)len < out_size; i++) {
            len += snprintf(out + len, out_size - (size_t)len, "%s\n", network_dns_get(i));
        }
    }
}

static void draw_transparent(cairo_t *cr, void *user_data) {
    (void)user_data;
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

static void draw_popup(cairo_t *cr, void *user_data) {
    (void)user_data;
    module_box_draw_rect(cr, 0, 0, g_popup.popup_width, g_popup.popup_height,
                          COLORS.mantle, COLORS.teal, (double)BAR.module_border_width);
    module_box_draw_text_left(cr, OUTER_MARGIN, OUTER_MARGIN, g_popup.popup_height - OUTER_MARGIN * 2.0,
                               g_popup.body, FONT.label, false, COLORS.teal);
}

static void create_catchers(AppContext *app) {
    g_popup.catcher_count = 0;
    for (Output *o = app->outputs; o && g_popup.catcher_count < MAX_TRACKED_OUTPUTS; o = o->next) {
        PopupSurface *catcher = popup_surface_create(
            app, o, 0, 0, 0, 0, 0, 0,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
            ZWLR_LAYER_SHELL_V1_LAYER_TOP);
        if (!catcher) {
            continue;
        }
        popup_surface_render_now(catcher, draw_transparent, NULL);
        g_popup.catchers[g_popup.catcher_count] = catcher;
        g_popup.catcher_surfaces[g_popup.catcher_count] = popup_surface_wl_surface(catcher);
        g_popup.catcher_count++;
    }
}

static void destroy_catchers(void) {
    for (int i = 0; i < g_popup.catcher_count; i++) {
        popup_surface_destroy(g_popup.catchers[i]);
        g_popup.catchers[i] = NULL;
        g_popup.catcher_surfaces[i] = NULL;
    }
    g_popup.catcher_count = 0;
}

void network_info_popup_open(Output *output, double box_x, double box_y, double box_w, double box_h) {
    network_info_popup_close(); // at most one network info popup open at a time

    g_popup.output = output;
    build_body(g_popup.body, sizeof(g_popup.body));

    double text_w, text_h;
    module_box_measure_text(g_popup.body, FONT.label, false, &text_w, &text_h);
    g_popup.popup_width = text_w + OUTER_MARGIN * 2.0;
    g_popup.popup_height = text_h + OUTER_MARGIN * 2.0;

    // Same TOP+RIGHT anchor+margin math (including the BAR.height
    // correction) as audio/device_popup.c and modules/tray/tray_context_menu.c
    // -- see CLAUDE.md's "D-Bus / system tray" section for the reasoning.
    // Right-anchored, unlike sysmon/sysmon_info_popup.c's own TOP+LEFT,
    // because the network widget lives in the bar's *right* region.
    int top_margin = (int)(box_y + box_h) - BAR.height;
    int right_margin = output->width - (int)(box_x + box_w);
    if (right_margin < 0) {
        right_margin = 0;
    }

    g_popup.popup = popup_surface_create(output->app, output, (int)g_popup.popup_width, (int)g_popup.popup_height,
                                          top_margin, right_margin, 0, 0,
                                          ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
                                          ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
    if (!g_popup.popup) {
        return;
    }

    create_catchers(output->app);
    g_popup.open = true;
    popup_surface_render_now(g_popup.popup, draw_popup, NULL);
}

void network_info_popup_close(void) {
    if (!g_popup.open) {
        return;
    }
    popup_surface_destroy(g_popup.popup);
    g_popup.popup = NULL;
    destroy_catchers();
    g_popup.open = false;
    g_popup.output = NULL;
}

bool network_info_popup_is_menu_surface(struct wl_surface *surface) {
    return g_popup.open && g_popup.popup && popup_surface_wl_surface(g_popup.popup) == surface;
}

bool network_info_popup_is_catcher_surface(struct wl_surface *surface) {
    if (!g_popup.open) {
        return false;
    }
    for (int i = 0; i < g_popup.catcher_count; i++) {
        if (g_popup.catcher_surfaces[i] == surface) {
            return true;
        }
    }
    return false;
}
