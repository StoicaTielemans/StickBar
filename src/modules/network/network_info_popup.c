#include "modules/network/network_info_popup.h"

#include <stdio.h>

#include "bar/popup_host.h"
#include "bar/popup_surface.h"
#include "config/glyphs.h"
#include "modules/network/network.h"
#include "network/network_client.h"
#include "render/braille_graph.h"
#include "render/color.h"
#include "render/module_box.h"
#include "render/popup_chrome.h"
#include "render/popup_style.h"
#include "render/stat_card.h"

// Layout tuning, local to this popup (see CLAUDE.md "Conventions").
#define GRAPH_CENTER_GAP 5.0 // between the download and upload halves; the
                             // centre line runs through the middle of it
#define GRAPH_CELLS_H 4                         // per half
#define GRID_COL_GAP 12.0
#define TAG_H 16.0
#define TAG_PAD_X 6.0
#define TAG_GAP 5.0

// Graph scale floor: below this the graph doesn't stretch any further, so
// an idle connection's background trickle (a few hundred B/s of ARP, mDNS,
// keepalives) stays a row of single dots instead of filling the graph.
#define GRAPH_MIN_SCALE (10.0 * 1024.0)

typedef struct {
    PopupHost host; // the dropdown surface, its placement and click-catchers
    double popup_width, popup_height;

} InfoPopupState;

static InfoPopupState g_popup;

// Matches NetworkMonitor.qml's own formatBytes() thresholds/precision
// exactly (2 decimal places on GB/MB/KB) -- deliberately a different
// precision from modules/network/network.c's network_format_rate() (1 decimal place), same as the
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

static const char *or_dash(const char *s) {
    return (s && s[0]) ? s : "-";
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
        double lw = module_box_text_width(labels[i], FONT.xs, true);
        if (lw > w) {
            w = lw;
        }
    }
    return w;
}

// ---- Painting ----------------------------------------------------------------

// Highest rate in the history -- shown as the "peak" label, and the graph
// half's scale (above GRAPH_MIN_SCALE), so the tallest column reaches the top.
// Peak over the samples from `first` on (the ones the graph shows).
static double history_peak(bool rx, int first) {
    double peak = 0.0;
    int n = network_history_len();
    for (int i = first; i < n; i++) {
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
    network_format_rate(rx ? network_rx_rate_bytes_per_sec() : network_tx_rate_bytes_per_sec(), rate, sizeof(rate));
    network_format_rate(peak, peak_rate, sizeof(peak_rate));
    snprintf(label, sizeof(label), "%s %s", arrow, rate);
    snprintf(peak_label, sizeof(peak_label), "peak %s", peak_rate);
    stat_graph_label_row(cr, x, y, w, label, rx ? COLORS.teal : COLORS.mauve, peak_label);
}

// Graph card: download label, download dots rising toward the centre line,
// the centre line, upload dots hanging below it, upload label.
static double paint_graph_card(cairo_t *cr, double x, double y, double w) {
    int cells = stat_graph_cells(w, NETWORK_HISTORY_LEN);
    double graph_w = braille_graph_width(cells);
    double half_h = braille_graph_height(GRAPH_CELLS_H);
    double h = STAT_CARD_PAD * 2.0 + STAT_GRAPH_LABEL_H * 2.0 + STAT_GRAPH_LABEL_GAP * 2.0 + half_h * 2.0 +
               GRAPH_CENTER_GAP;
    if (!cr) {
        return h;
    }

    popup_draw_card(cr, x, y, w, h, false, false, COLORS.surface1);

    double rx_samples[NETWORK_HISTORY_LEN], tx_samples[NETWORK_HISTORY_LEN];
    // Only the samples the graph has room for (and so the peaks over them).
    int n;
    int first = stat_history_window(network_history_len(), cells, &n);
    for (int i = 0; i < n; i++) {
        rx_samples[i] = network_rx_history(first + i);
        tx_samples[i] = network_tx_history(first + i);
    }
    double rx_peak = history_peak(true, first), tx_peak = history_peak(false, first);
    double rx_scale = rx_peak > GRAPH_MIN_SCALE ? rx_peak : GRAPH_MIN_SCALE;
    double tx_scale = tx_peak > GRAPH_MIN_SCALE ? tx_peak : GRAPH_MIN_SCALE;

    double inner_x = x + STAT_CARD_PAD, inner_w = w - STAT_CARD_PAD * 2.0;
    double graph_x = x + (w - graph_w) / 2.0;
    double cy = y + STAT_CARD_PAD;

    draw_rate_label_row(cr, inner_x, cy, inner_w, true, rx_peak);
    cy += STAT_GRAPH_LABEL_H + STAT_GRAPH_LABEL_GAP;

    // btop-like height gradients: download in the module's teal warming to
    // yellow near its peak, upload in mauve warming to red.
    const Color rx_gradient[3] = {COLORS.teal, COLORS.green, COLORS.yellow};
    const Color tx_gradient[3] = {COLORS.mauve, COLORS.pink, COLORS.red};
    braille_graph_draw(cr, graph_x, cy, cells, GRAPH_CELLS_H, rx_samples, n, rx_scale, true, rx_gradient);
    cy += half_h;

    cairo_save(cr);
    color_set_source(cr, COLORS.surface1);
    cairo_rectangle(cr, graph_x, cy + GRAPH_CENTER_GAP / 2.0 - 0.5, graph_w, 1.0);
    cairo_fill(cr);
    cairo_restore(cr);
    cy += GRAPH_CENTER_GAP;

    braille_graph_draw(cr, graph_x, cy, cells, GRAPH_CELLS_H, tx_samples, n, tx_scale, false, tx_gradient);
    cy += half_h + STAT_GRAPH_LABEL_GAP;

    draw_rate_label_row(cr, inner_x, cy, inner_w, false, tx_peak);
    return h;
}

static double paint_header(cairo_t *cr, double x, double y, double w) {
    if (!cr) {
        return POPUP_HEADER_H;
    }
    char icon[5];
    glyph_utf8(network_status_glyph(), icon);
    PopupHeader header = {.icon = icon, .icon_color = COLORS.teal, .title = "No active connection"};
    const char *iface = network_interface_name();
    if (iface[0]) {
        bool up = network_connected();
        header.title = iface;
        header.summary = network_is_wifi() ? "  ·  Wi-Fi" : "  ·  Ethernet";
        header.pill = up ? "connected" : "down";
        header.pill_fill = up ? COLORS.teal : COLORS.red;
    }
    popup_draw_header(cr, x, y, w, &header);
    return POPUP_HEADER_H;
}

// DNS servers as small tags, wrapped onto as many rows as the value column
// needs. Returns the height used (at least one grid row, even with no
// servers, so the label still has its row).
static double paint_dns_tags(cairo_t *cr, double x, double y, double w) {
    int count = network_dns_count();
    double row_y = 0.0, tag_x = 0.0;
    for (int i = 0; i < count; i++) {
        const char *server = network_dns_get(i);
        double tag_w = module_box_text_width(server, FONT.sm, false) + TAG_PAD_X * 2.0;
        if (tag_x > 0.0 && tag_x + tag_w > w) {
            tag_x = 0.0;
            row_y += TAG_H + TAG_GAP;
        }
        if (cr) {
            double ty = y + row_y + (STAT_ROW_H - TAG_H) / 2.0;
            module_box_draw_rect(cr, x + tag_x, ty, tag_w, TAG_H, COLORS.surface0, COLORS.surface1, 1.0);
            module_box_draw_text_centered(cr, x + tag_x, ty, tag_w, TAG_H, server, FONT.sm, false, COLORS.text);
        }
        tag_x += tag_w + TAG_GAP;
    }
    if (count == 0 && cr) {
        module_box_draw_text_left(cr, x, y, STAT_ROW_H, "-", FONT.label, false, COLORS.text);
    }
    return row_y + STAT_ROW_H;
}

static double paint_details(cairo_t *cr, double x, double y, double w) {
    GridRow rows[MAX_GRID_ROWS];
    int n = build_grid_rows(rows);
    double label_w = grid_label_col_w();
    double value_x = x + label_w + GRID_COL_GAP;
    double cy = y;
    for (int i = 0; i < n; i++) {
        if (cr) {
            module_box_draw_text_left(cr, x, cy, STAT_ROW_H, rows[i].label, FONT.xs, true, COLORS.overlay0);
            module_box_draw_text_left(cr, value_x, cy, STAT_ROW_H, rows[i].value, FONT.label, false, COLORS.text);
        }
        cy += STAT_ROW_H;
    }
    if (cr) {
        module_box_draw_text_left(cr, x, cy, STAT_ROW_H, "DNS", FONT.xs, true, COLORS.overlay0);
    }
    cy += paint_dns_tags(cr, value_x, cy, x + w - value_x);
    return cy - y;
}

// Lays out (and, with a non-NULL `cr`, draws) the whole popup content at
// content width `w`; returns its total height. One function for both so the
// measured size and the drawn layout can't drift apart.
static double paint_content(cairo_t *cr, double w) {
    double x = POPUP_OUTER_MARGIN, y = POPUP_OUTER_MARGIN;
    y += paint_header(cr, x, y, w);
    if (network_interface_name()[0]) {
        y += POPUP_SECTION_GAP;
        y += paint_graph_card(cr, x, y, w);
        y += stat_graph_axis(cr, x, y, w, stat_graph_cells(w, NETWORK_HISTORY_LEN)) + POPUP_SECTION_GAP;
        y += paint_details(cr, x, y, w);
    }
    return y + POPUP_OUTER_MARGIN;
}

// Content width: the graph card's natural width, widened if the header or
// a details row needs more.
static double content_width(void) {
    double w = braille_graph_width(STAT_GRAPH_MIN_CELLS_W) + STAT_CARD_PAD * 2.0;

    char icon[5];
    glyph_utf8(network_status_glyph(), icon);
    // Sized for the wider of each varying part.
    PopupHeader header = {.icon = icon, .title = network_interface_name(), .summary = "  ·  Ethernet",
                          .pill = "connected"};
    if (!network_interface_name()[0]) {
        header = (PopupHeader){.icon = icon, .title = "No active connection"};
    }
    double header_w = popup_header_width(&header);
    if (header_w > w) {
        w = header_w;
    }

    GridRow rows[MAX_GRID_ROWS];
    int n = build_grid_rows(rows);
    double value_x = grid_label_col_w() + GRID_COL_GAP;
    for (int i = 0; i < n; i++) {
        double row_w = value_x + module_box_text_width(rows[i].value, FONT.label, false);
        if (row_w > w) {
            w = row_w;
        }
    }
    return w;
}

static void draw_popup(cairo_t *cr, void *user_data) {
    (void)user_data;
    // Fully repainted every time (opaque fill first), so the ping-ponged shm
    // buffer's stale frame never shows through -- see
    // docs/modules/power-menu.md's note on CAIRO_OPERATOR_SOURCE for the translucent case this avoids.
    popup_draw_frame(cr, g_popup.popup_width, g_popup.popup_height, COLORS.teal);
    paint_content(cr, g_popup.popup_width - POPUP_OUTER_MARGIN * 2.0);
}

void network_info_popup_open(Output *output, double box_x, double box_y, double box_w, double box_h) {
    network_client_refresh_details(); // the 5s poll keeps only what the bar box shows
    network_info_popup_close(); // at most one network info popup open at a time

    g_popup.host.output = output;

    // Size is fixed at open: the layout only changes shape if the
    // interface, its type or the DNS list changes while open, which is rare
    // -- live refreshes then just clip, and reopening picks up the new size.
    double content_w = content_width();
    g_popup.popup_width = content_w + POPUP_OUTER_MARGIN * 2.0;
    g_popup.popup_height = paint_content(NULL, content_w);

    // Under its box, opening leftward from the box's right edge (bar/popup_host.c).
    if (!popup_host_open(&g_popup.host, output, POPUP_ANCHOR_RIGHT, box_x, box_y, box_w, box_h,
                         (int)g_popup.popup_width, (int)g_popup.popup_height)) {
        return;
    }
    popup_surface_render_now(g_popup.host.popup, draw_popup, NULL);
}

void network_info_popup_refresh_if_open(void) {
    if (!g_popup.host.open) {
        return;
    }
    network_client_refresh_details();
    popup_surface_render_now(g_popup.host.popup, draw_popup, NULL);
}

void network_info_popup_close(void) {
    if (!g_popup.host.open) {
        return;
    }
    popup_host_close(&g_popup.host);
}

bool network_info_popup_is_menu_surface(struct wl_surface *surface) {
    return popup_host_is_menu_surface(&g_popup.host, surface);
}

bool network_info_popup_is_catcher_surface(struct wl_surface *surface) {
    return popup_host_is_catcher_surface(&g_popup.host, surface);
}
