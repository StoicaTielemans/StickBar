#include "modules/network/network_info_popup.h"

#include <stdio.h>

#include "bar/popup_surface.h"
#include "config/glyphs.h"
#include "network/network_client.h"
#include "render/braille_graph.h"
#include "render/module_box.h"

// Sized generously (this machine has 2 monitors), matching the same
// MAX_TRACKED_OUTPUTS convention used by every other popup in this project.
#define MAX_TRACKED_OUTPUTS 8

// Layout tuning, local to this popup (see CLAUDE.md "Conventions").
#define OUTER_MARGIN 12.0
#define SECTION_GAP 10.0
#define HEADER_H 22.0
#define PILL_H 16.0
#define PILL_PAD_X 7.0
#define CARD_PAD 8.0
#define GRAPH_LABEL_H 14.0
#define GRAPH_LABEL_GAP 3.0
#define GRAPH_CENTER_GAP 5.0 // between the download and upload halves; the
                             // centre line runs through the middle of it
#define GRAPH_CELLS_W (NETWORK_HISTORY_LEN / 2) // one sample per dot column
#define GRAPH_CELLS_H 4                         // per half
#define AXIS_H 12.0
#define GRID_ROW_H 17.0
#define GRID_COL_GAP 12.0
#define TAG_H 16.0
#define TAG_PAD_X 6.0
#define TAG_GAP 5.0

// Graph scale floor: below this the graph doesn't stretch any further, so
// an idle connection's background trickle (a few hundred B/s of ARP, mDNS,
// keepalives) stays a row of single dots instead of filling the graph.
#define GRAPH_MIN_SCALE (10.0 * 1024.0)

typedef struct {
    bool open;
    Output *output;
    PopupSurface *popup;
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

// Same choice as modules/network/network.c's status_icon_glyph() (local
// copy, per this project's usual per-file duplication of tiny helpers).
static uint32_t status_icon_glyph(void) {
    if (network_interface_name()[0] == '\0') {
        return GLYPH_ICON_NETWORK_UNKNOWN;
    }
    if (!network_connected()) {
        return network_is_wifi() ? GLYPH_ICON_WIFI_OFF : GLYPH_ICON_ETHERNET_OFF;
    }
    return network_is_wifi() ? GLYPH_ICON_WIFI_ON : GLYPH_ICON_ETHERNET_ON;
}

static double text_width(const char *text, int size, bool bold) {
    double w, h;
    module_box_measure_text(text, size, bold, &w, &h);
    (void)h;
    return w;
}

static void draw_text_right(cairo_t *cr, double right_x, double y, double h, const char *text, int size, bool bold,
                            Color color) {
    module_box_draw_text_left(cr, right_x - text_width(text, size, bold), y, h, text, size, bold, color);
}

// ---- Details grid ----------------------------------------------------------

// One label/value row of the details grid. Built fresh on every paint (the
// values change on every poll), as a fixed-size table rather than anything
// allocated.
#define MAX_GRID_ROWS 8

typedef struct {
    const char *label;
    char value[96];
} GridRow;

static int build_grid_rows(GridRow rows[MAX_GRID_ROWS]) {
    int n = 0;
    rows[n].label = "IP";
    snprintf(rows[n++].value, sizeof(rows[0].value), "%s", or_dash(network_ip_address()));
    rows[n].label = "GATEWAY";
    snprintf(rows[n++].value, sizeof(rows[0].value), "%s", or_dash(network_gateway()));
    rows[n].label = "MAC";
    snprintf(rows[n++].value, sizeof(rows[0].value), "%s", or_dash(network_mac_address()));
    rows[n].label = "MTU";
    snprintf(rows[n++].value, sizeof(rows[0].value), "%s", or_dash(network_mtu()));

    // Link speed/duplex don't apply to Wi-Fi -- the same omission
    // NetworkTooltip.qml's buildTooltip() makes.
    if (!network_is_wifi()) {
        const char *speed = network_link_speed();
        rows[n].label = "LINK";
        if (speed[0]) {
            snprintf(rows[n++].value, sizeof(rows[0].value), "%s Mbps, %s duplex", speed,
                     or_dash(network_duplex()));
        } else {
            snprintf(rows[n++].value, sizeof(rows[0].value), "-");
        }
    }

    // Interface byte counters from /sys -- totals since the interface came
    // up (normally boot), not since the bar started.
    char down_buf[5], up_buf[5], rx_total[32], tx_total[32];
    glyph_utf8(GLYPH_ARROW_DOWN, down_buf);
    glyph_utf8(GLYPH_ARROW_UP, up_buf);
    format_bytes(network_rx_total_bytes(), rx_total, sizeof(rx_total));
    format_bytes(network_tx_total_bytes(), tx_total, sizeof(tx_total));
    rows[n].label = "SINCE BOOT";
    snprintf(rows[n++].value, sizeof(rows[0].value), "%s %s   %s %s", down_buf, rx_total, up_buf, tx_total);
    return n;
}

static double grid_label_col_w(void) {
    // Widest label any row can have, so the value column doesn't shift
    // between Wi-Fi and Ethernet or as values change.
    static const char *labels[] = {"IP", "GATEWAY", "MAC", "MTU", "LINK", "SINCE BOOT", "DNS"};
    double w = 0.0;
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        double lw = text_width(labels[i], FONT.xs, true);
        if (lw > w) {
            w = lw;
        }
    }
    return w;
}

// ---- Painting ----------------------------------------------------------------

// Highest rate in the history -- shown as the "peak" label, and the graph
// half's scale (above GRAPH_MIN_SCALE), so the tallest column reaches the top.
static double history_peak(bool rx) {
    double peak = 0.0;
    int n = network_history_len();
    for (int i = 0; i < n; i++) {
        double v = rx ? network_rx_history(i) : network_tx_history(i);
        if (v > peak) {
            peak = v;
        }
    }
    return peak;
}

static void draw_rate_label_row(cairo_t *cr, double x, double y, double w, bool rx, double peak) {
    char arrow[5], rate[32], peak_rate[32], label[48], peak_label[48];
    glyph_utf8(rx ? GLYPH_ARROW_DOWN : GLYPH_ARROW_UP, arrow);
    format_rate(rx ? network_rx_rate_bytes_per_sec() : network_tx_rate_bytes_per_sec(), rate, sizeof(rate));
    format_rate(peak, peak_rate, sizeof(peak_rate));
    snprintf(label, sizeof(label), "%s %s", arrow, rate);
    snprintf(peak_label, sizeof(peak_label), "peak %s", peak_rate);
    module_box_draw_text_left(cr, x, y, GRAPH_LABEL_H, label, FONT.sm, true, rx ? COLORS.teal : COLORS.mauve);
    draw_text_right(cr, x + w, y, GRAPH_LABEL_H, peak_label, FONT.xs, false, COLORS.overlay0);
}

// Graph card: download label, download dots rising toward the centre line,
// the centre line, upload dots hanging below it, upload label.
static double paint_graph_card(cairo_t *cr, double x, double y, double w) {
    double graph_w = braille_graph_width(GRAPH_CELLS_W);
    double half_h = braille_graph_height(GRAPH_CELLS_H);
    double h = CARD_PAD * 2.0 + GRAPH_LABEL_H * 2.0 + GRAPH_LABEL_GAP * 2.0 + half_h * 2.0 + GRAPH_CENTER_GAP;
    if (!cr) {
        return h;
    }

    module_box_draw_rect(cr, x, y, w, h, COLORS.base, COLORS.surface1, 1.0);

    double rx_samples[NETWORK_HISTORY_LEN], tx_samples[NETWORK_HISTORY_LEN];
    int n = network_history_len();
    for (int i = 0; i < n; i++) {
        rx_samples[i] = network_rx_history(i);
        tx_samples[i] = network_tx_history(i);
    }
    double rx_peak = history_peak(true), tx_peak = history_peak(false);
    double rx_scale = rx_peak > GRAPH_MIN_SCALE ? rx_peak : GRAPH_MIN_SCALE;
    double tx_scale = tx_peak > GRAPH_MIN_SCALE ? tx_peak : GRAPH_MIN_SCALE;

    double inner_x = x + CARD_PAD, inner_w = w - CARD_PAD * 2.0;
    double graph_x = x + (w - graph_w) / 2.0;
    double cy = y + CARD_PAD;

    draw_rate_label_row(cr, inner_x, cy, inner_w, true, rx_peak);
    cy += GRAPH_LABEL_H + GRAPH_LABEL_GAP;

    // btop-like height gradients: download in the module's teal warming to
    // yellow near its peak, upload in mauve warming to red.
    const Color rx_gradient[3] = {COLORS.teal, COLORS.green, COLORS.yellow};
    const Color tx_gradient[3] = {COLORS.mauve, COLORS.pink, COLORS.red};
    braille_graph_draw(cr, graph_x, cy, GRAPH_CELLS_W, GRAPH_CELLS_H, rx_samples, n, rx_scale, true, rx_gradient);
    cy += half_h;

    cairo_save(cr);
    cairo_set_source_rgba(cr, COLORS.surface1.r, COLORS.surface1.g, COLORS.surface1.b, COLORS.surface1.a);
    cairo_rectangle(cr, graph_x, cy + GRAPH_CENTER_GAP / 2.0 - 0.5, graph_w, 1.0);
    cairo_fill(cr);
    cairo_restore(cr);
    cy += GRAPH_CENTER_GAP;

    braille_graph_draw(cr, graph_x, cy, GRAPH_CELLS_W, GRAPH_CELLS_H, tx_samples, n, tx_scale, false, tx_gradient);
    cy += half_h + GRAPH_LABEL_GAP;

    draw_rate_label_row(cr, inner_x, cy, inner_w, false, tx_peak);
    return h;
}

static double paint_header(cairo_t *cr, double x, double y, double w) {
    if (!cr) {
        return HEADER_H;
    }
    char icon[5];
    glyph_utf8(status_icon_glyph(), icon);
    module_box_draw_text_left(cr, x, y, HEADER_H, icon, FONT.heading, false, COLORS.teal);
    double tx = x + text_width(icon, FONT.heading, false) + 8.0;

    const char *iface = network_interface_name();
    if (!iface[0]) {
        module_box_draw_text_left(cr, tx, y, HEADER_H, "No active connection", FONT.md, true, COLORS.text);
        return HEADER_H;
    }
    module_box_draw_text_left(cr, tx, y, HEADER_H, iface, FONT.md, true, COLORS.text);
    tx += text_width(iface, FONT.md, true);
    module_box_draw_text_left(cr, tx, y, HEADER_H, network_is_wifi() ? "  ·  Wi-Fi" : "  ·  Ethernet", FONT.sm,
                              false, COLORS.subtext0);

    // Status pill: filled, borderless, at the header's right edge.
    bool up = network_connected();
    const char *pill_text = up ? "connected" : "down";
    double pill_w = text_width(pill_text, FONT.xs, true) + PILL_PAD_X * 2.0;
    double pill_x = x + w - pill_w;
    double pill_y = y + (HEADER_H - PILL_H) / 2.0;
    module_box_draw_rect(cr, pill_x, pill_y, pill_w, PILL_H, up ? COLORS.teal : COLORS.red, COLORS.mantle, 0.0);
    module_box_draw_text_centered(cr, pill_x, pill_y, pill_w, PILL_H, pill_text, FONT.xs, true, COLORS.mantle);
    return HEADER_H;
}

// DNS servers as small tags, wrapped onto as many rows as the value column
// needs. Returns the height used (at least one grid row, even with no
// servers, so the label still has its row).
static double paint_dns_tags(cairo_t *cr, double x, double y, double w) {
    int count = network_dns_count();
    double row_y = 0.0, tag_x = 0.0;
    for (int i = 0; i < count; i++) {
        const char *server = network_dns_get(i);
        double tag_w = text_width(server, FONT.sm, false) + TAG_PAD_X * 2.0;
        if (tag_x > 0.0 && tag_x + tag_w > w) {
            tag_x = 0.0;
            row_y += TAG_H + TAG_GAP;
        }
        if (cr) {
            double ty = y + row_y + (GRID_ROW_H - TAG_H) / 2.0;
            module_box_draw_rect(cr, x + tag_x, ty, tag_w, TAG_H, COLORS.surface0, COLORS.surface1, 1.0);
            module_box_draw_text_centered(cr, x + tag_x, ty, tag_w, TAG_H, server, FONT.sm, false, COLORS.text);
        }
        tag_x += tag_w + TAG_GAP;
    }
    if (count == 0 && cr) {
        module_box_draw_text_left(cr, x, y, GRID_ROW_H, "-", FONT.label, false, COLORS.text);
    }
    return row_y + GRID_ROW_H;
}

static double paint_details(cairo_t *cr, double x, double y, double w) {
    GridRow rows[MAX_GRID_ROWS];
    int n = build_grid_rows(rows);
    double label_w = grid_label_col_w();
    double value_x = x + label_w + GRID_COL_GAP;
    double cy = y;
    for (int i = 0; i < n; i++) {
        if (cr) {
            module_box_draw_text_left(cr, x, cy, GRID_ROW_H, rows[i].label, FONT.xs, true, COLORS.overlay0);
            module_box_draw_text_left(cr, value_x, cy, GRID_ROW_H, rows[i].value, FONT.label, false, COLORS.text);
        }
        cy += GRID_ROW_H;
    }
    if (cr) {
        module_box_draw_text_left(cr, x, cy, GRID_ROW_H, "DNS", FONT.xs, true, COLORS.overlay0);
    }
    cy += paint_dns_tags(cr, value_x, cy, x + w - value_x);
    return cy - y;
}

// Lays out (and, with a non-NULL `cr`, draws) the whole popup content at
// content width `w`; returns its total height. One function for both so the
// measured size and the drawn layout can't drift apart.
static double paint_content(cairo_t *cr, double w) {
    double x = OUTER_MARGIN, y = OUTER_MARGIN;
    y += paint_header(cr, x, y, w);
    if (network_interface_name()[0]) {
        y += SECTION_GAP;
        y += paint_graph_card(cr, x, y, w);
        if (cr) {
            module_box_draw_text_left(cr, x, y + 2.0, AXIS_H, "5 min ago", FONT.xs, false, COLORS.overlay0);
            draw_text_right(cr, x + w, y + 2.0, AXIS_H, "now", FONT.xs, false, COLORS.overlay0);
        }
        y += AXIS_H + 2.0 + SECTION_GAP;
        y += paint_details(cr, x, y, w);
    }
    return y + OUTER_MARGIN;
}

// Content width: the graph card's natural width, widened if the header or
// a details row needs more.
static double content_width(void) {
    double w = braille_graph_width(GRAPH_CELLS_W) + CARD_PAD * 2.0;

    char icon[5];
    glyph_utf8(status_icon_glyph(), icon);
    double header_w = text_width(icon, FONT.heading, false) + 8.0 +
                      text_width(network_interface_name(), FONT.md, true) +
                      text_width("  ·  Ethernet", FONT.sm, false) + 16.0 +
                      text_width("connected", FONT.xs, true) + PILL_PAD_X * 2.0;
    if (!network_interface_name()[0]) {
        header_w = text_width(icon, FONT.heading, false) + 8.0 + text_width("No active connection", FONT.md, true);
    }
    if (header_w > w) {
        w = header_w;
    }

    GridRow rows[MAX_GRID_ROWS];
    int n = build_grid_rows(rows);
    double value_x = grid_label_col_w() + GRID_COL_GAP;
    for (int i = 0; i < n; i++) {
        double row_w = value_x + text_width(rows[i].value, FONT.label, false);
        if (row_w > w) {
            w = row_w;
        }
    }
    return w;
}

static void draw_popup(cairo_t *cr, void *user_data) {
    (void)user_data;
    // Fully repainted every time (opaque fill first), so the ping-ponged shm
    // buffer's stale frame never shows through -- see CLAUDE.md's power-menu
    // note on CAIRO_OPERATOR_SOURCE for the translucent case this avoids.
    module_box_draw_rect(cr, 0, 0, g_popup.popup_width, g_popup.popup_height,
                          COLORS.mantle, COLORS.teal, (double)BAR.module_border_width);
    paint_content(cr, g_popup.popup_width - OUTER_MARGIN * 2.0);
}

static void create_catchers(AppContext *app) {
    g_popup.catcher_count = 0;
    for (Output *o = app->outputs; o && g_popup.catcher_count < MAX_TRACKED_OUTPUTS; o = o->next) {
        PopupSurface *catcher = popup_surface_create_catcher(app, o);
        if (!catcher) {
            continue;
        }
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

    // Size is fixed at open: the layout only changes shape if the
    // interface, its type or the DNS list changes while open, which is rare
    // -- live refreshes then just clip, and reopening picks up the new size.
    double content_w = content_width();
    g_popup.popup_width = content_w + OUTER_MARGIN * 2.0;
    g_popup.popup_height = paint_content(NULL, content_w);

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

void network_info_popup_refresh_if_open(void) {
    if (!g_popup.open) {
        return;
    }
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
