#include "modules/mem/mem_popup.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bar/popup_host.h"
#include "bar/popup_surface.h"
#include "config/glyphs.h"
#include "modules/cpu/cpu_popup.h"
#include "modules/gpu/gpu_popup.h"
#include "render/braille_graph.h"
#include "render/color.h"
#include "render/meter_bar.h"
#include "render/module_box.h"
#include "render/popup_chrome.h"
#include "render/popup_style.h"
#include "render/stat_card.h"
#include "sysmon/sysmon_client.h"

// Layout tuning, local to this popup (see CLAUDE.md "Conventions"). Section
// rhythm and graph card match modules/cpu/cpu_popup.c and gpu_popup.c on
// purpose, so the three sysmon dropdowns read as a set.
#define USED_CELLS_H 4
#define SWAP_CELLS_H 2
#define SPLIT_BAR_H 8.0
#define SPLIT_SEGMENTS 48
#define SPLIT_SEGMENT_GAP 1.0 // same as render/meter_bar.c's
#define LEGEND_SWATCH 7.0
#define LEGEND_GAP 14.0
#define METER_W 110.0
#define METER_H 6.0
#define METER_SEGMENTS 12

typedef struct {
    PopupHost host; // the dropdown surface, its placement and click-catchers
    double popup_width, popup_height;

} MemPopupState;

static MemPopupState g_popup;

// Yellow (the module's accent) -> peach -> red as memory fills; a function
// since COLORS isn't a compile-time constant (see docs/modules/power-menu.md).
static void fill_gradient(Color out[3]) {
    out[0] = COLORS.yellow;
    out[1] = COLORS.peach;
    out[2] = COLORS.red;
}

// "1.2 GB" above a gigabyte, "830 MB" below, so small values (dirty pages,
// zram) don't all read "0.0 GB".
static void format_gb(char *out, size_t out_size, double gb) {
    if (gb >= 1.0) {
        snprintf(out, out_size, "%.1f GB", gb);
    } else {
        snprintf(out, out_size, "%.0f MB", gb * 1024.0);
    }
}

static void header_text(char *out, size_t out_size) {
    const char *swap = sysmon_swap_total_gb() <= 0.0 ? "no swap" : sysmon_swap_is_zram() ? "zram swap" : "swap";
    snprintf(out, out_size, "  ·  %.1f GB  ·  %s", sysmon_mem_total_gb(), swap);
}

// Pressure pill. PSI says whether anything is actually *stalling* on memory,
// which percent used can't: this desktop routinely sits at ~70% "used" by
// the kernel's measure with zero stalls, because most of it is cache. Without
// PSI, fall back to percent used.
static void pill_state(const char **word, Color *fill) {
    if (sysmon_mem_psi_available()) {
        double some = sysmon_mem_psi_some(0);
        *word = some < 1.0 ? "ok" : some < 10.0 ? "pressure" : "thrashing";
        *fill = some < 1.0 ? COLORS.green : some < 10.0 ? COLORS.yellow : COLORS.red;
        return;
    }
    int pct = sysmon_mem_percent();
    *word = pct < 70 ? "ok" : pct < 90 ? "high" : "full";
    *fill = pct < 70 ? COLORS.green : pct < 90 ? COLORS.yellow : COLORS.red;
}

// ---- Painting ----------------------------------------------------------------
// Every paint_* takes a NULL `cr` to mean "measure only" and returns its
// height, so the size computed at open and the drawn layout can't drift.

static double paint_header(cairo_t *cr, double x, double y, double w) {
    if (!cr) {
        return POPUP_HEADER_H;
    }
    char icon[5], rest[96];
    glyph_utf8(GLYPH_ICON_MEM, icon);
    header_text(rest, sizeof(rest));
    PopupHeader header = {.icon = icon, .icon_color = COLORS.yellow, .title = "Memory", .summary = rest};
    pill_state(&header.pill, &header.pill_fill);
    popup_draw_header(cr, x, y, w, &header);
    return POPUP_HEADER_H;
}

static double paint_graph_card(cairo_t *cr, double x, double y, double w) {
    int cells = stat_graph_cells(w, SYSMON_MEM_HISTORY_LEN);
    double graph_w = braille_graph_width(cells);
    double used_h = braille_graph_height(USED_CELLS_H);
    double swap_h = braille_graph_height(SWAP_CELLS_H);
    double h = STAT_CARD_PAD * 2.0 + (STAT_GRAPH_LABEL_H + STAT_GRAPH_LABEL_GAP) * 2.0 + used_h + STAT_GRAPH_SPLIT_GAP +
               swap_h;
    if (!cr) {
        return h;
    }
    popup_draw_card(cr, x, y, w, h, false, false, COLORS.surface1);

    // Only the samples the graph has room for (and so the peaks over them).
    int n;
    int first = stat_history_window(sysmon_mem_history_len(), cells, &n);
    double used[SYSMON_MEM_HISTORY_LEN], swap[SYSMON_MEM_HISTORY_LEN];
    // The labels show the fresh figures from sysmon_mem_refresh_details(),
    // which can be newer than the last history sample -- so the peak starts
    // at them, never reading lower than the value beside it.
    double used_peak = sysmon_mem_percent(), swap_peak = sysmon_swap_percent();
    for (int i = 0; i < n; i++) {
        used[i] = sysmon_mem_used_history(first + i);
        swap[i] = sysmon_mem_swap_history(first + i);
        if (used[i] > used_peak) {
            used_peak = used[i];
        }
        if (swap[i] > swap_peak) {
            swap_peak = swap[i];
        }
    }

    double inner_x = x + STAT_CARD_PAD, inner_w = w - STAT_CARD_PAD * 2.0;
    double graph_x = x + (w - graph_w) / 2.0;
    double cy = y + STAT_CARD_PAD;
    char left[48], right[48];
    Color gradient[3];
    fill_gradient(gradient);

    snprintf(left, sizeof(left), "used %d%%", sysmon_mem_percent());
    snprintf(right, sizeof(right), "peak %.0f%%", used_peak);
    stat_graph_label_row(cr, inner_x, cy, inner_w, left, COLORS.yellow, right);
    cy += STAT_GRAPH_LABEL_H + STAT_GRAPH_LABEL_GAP;
    braille_graph_draw(cr, graph_x, cy, cells, USED_CELLS_H, used, n, 100.0, true, gradient);
    cy += used_h + STAT_GRAPH_SPLIT_GAP;

    snprintf(left, sizeof(left), "swap %d%%", sysmon_swap_percent());
    snprintf(right, sizeof(right), "peak %.0f%%", swap_peak);
    stat_graph_label_row(cr, inner_x, cy, inner_w, left, COLORS.yellow, right);
    cy += STAT_GRAPH_LABEL_H + STAT_GRAPH_LABEL_GAP;
    const Color swap_gradient[3] = {COLORS.peach, COLORS.maroon, COLORS.red};
    braille_graph_draw(cr, graph_x, cy, cells, SWAP_CELLS_H, swap, n, 100.0, true, swap_gradient);
    return h;
}

// One full-width segmented bar split into apps | cache | free, plus a legend.
// apps = total - available (the bar box's own "used"), cache = available -
// free (reclaimable), free = MemFree -- the three add up to the total, which
// shows at a glance why "free" is tiny while nothing is wrong.
static double paint_split(cairo_t *cr, double x, double y, double w) {
    double h = SPLIT_BAR_H + 4.0 + STAT_ROW_H;
    if (!cr) {
        return h;
    }
    double total = sysmon_mem_total_gb();
    double apps = sysmon_mem_used_gb(), cache = sysmon_mem_cache_gb();
    double apps_end = total > 0.0 ? apps / total : 0.0;
    double cache_end = total > 0.0 ? (apps + cache) / total : 0.0;
    double seg_w = (w - SPLIT_SEGMENT_GAP * (SPLIT_SEGMENTS - 1)) / SPLIT_SEGMENTS;

    // A segment takes the colour of whichever part covers its centre.
    cairo_save(cr);
    for (int i = 0; i < SPLIT_SEGMENTS; i++) {
        double centre = (i + 0.5) / SPLIT_SEGMENTS;
        Color c = centre < apps_end ? COLORS.yellow : centre < cache_end ? COLORS.blue : COLORS.surface1;
        color_set_source(cr, c);
        cairo_rectangle(cr, x + i * (seg_w + SPLIT_SEGMENT_GAP), y, seg_w, SPLIT_BAR_H);
        cairo_fill(cr);
    }
    cairo_restore(cr);

    double ly = y + SPLIT_BAR_H + 4.0;
    double lx = x;
    const char *names[] = {"apps", "cache", "free"};
    const double values[] = {apps, cache, sysmon_mem_free_gb()};
    const Color colors[] = {COLORS.yellow, COLORS.blue, COLORS.surface1};
    for (int i = 0; i < 3; i++) {
        module_box_draw_rect(cr, lx, ly + (STAT_ROW_H - LEGEND_SWATCH) / 2.0, LEGEND_SWATCH, LEGEND_SWATCH, colors[i],
                             COLORS.mantle, 0.0);
        lx += LEGEND_SWATCH + 5.0;
        module_box_draw_text_left(cr, lx, ly, STAT_ROW_H, names[i], FONT.xs, true, COLORS.overlay0);
        lx += module_box_text_width(names[i], FONT.xs, true) + 5.0;
        char value[24];
        format_gb(value, sizeof(value), values[i]);
        module_box_draw_text_left(cr, lx, ly, STAT_ROW_H, value, FONT.label, false, COLORS.text);
        lx += module_box_text_width(value, FONT.label, false) + LEGEND_GAP;
    }
    return h;
}

static const char *ROW_LABELS[] = {"RAM", "SWAP", "ZRAM", "AVAIL", "PRESS"};

static double label_col_w(void) {
    return stat_label_col_w(ROW_LABELS, sizeof(ROW_LABELS) / sizeof(ROW_LABELS[0]));
}

// The text rows after the two meters; ZRAM and PRESS only when available.
// Fixed at open (whether zram/PSI exist doesn't change while running).
static int text_row_count(void) {
    return 1 + (sysmon_swap_is_zram() ? 1 : 0) + (sysmon_mem_psi_available() ? 1 : 0);
}

static void zram_text(char *out, size_t out_size) {
    double orig = sysmon_zram_orig_mb(), compr = sysmon_zram_compr_mb();
    char ratio[16] = "";
    if (compr > 0.0) {
        snprintf(ratio, sizeof(ratio), " (%.1fx)", orig / compr);
    }
    char orig_s[24], compr_s[24], used_s[24];
    format_gb(orig_s, sizeof(orig_s), orig / 1024.0);
    format_gb(compr_s, sizeof(compr_s), compr / 1024.0);
    format_gb(used_s, sizeof(used_s), sysmon_zram_used_mb() / 1024.0);
    const char *algo = sysmon_zram_algorithm()[0] ? sysmon_zram_algorithm() : "zram";
    snprintf(out, out_size, "%s · %s -> %s%s · uses %s", algo, orig_s, compr_s, ratio, used_s);
}

static void avail_text(char *out, size_t out_size) {
    char avail[24], shared[24];
    format_gb(avail, sizeof(avail), sysmon_mem_avail_gb());
    format_gb(shared, sizeof(shared), sysmon_mem_shared_gb());
    snprintf(out, out_size, "%s · shared %s · dirty %.0f MB", avail, shared, sysmon_mem_dirty_mb());
}

static void psi_text(char *out, size_t out_size) {
    snprintf(out, out_size, "%.2f / %.2f / %.2f %%  (10s / 60s / 5m)", sysmon_mem_psi_some(0),
             sysmon_mem_psi_some(1), sysmon_mem_psi_some(2));
}

static void meter_row(cairo_t *cr, double x, double y, double w, const char *label, double used_gb,
                      double total_gb, int percent) {
    module_box_draw_text_left(cr, x, y, STAT_ROW_H, label, FONT.xs, true, COLORS.overlay0);
    double mx = x + label_col_w() + POPUP_COL_GAP;
    Color gradient[3];
    fill_gradient(gradient);
    meter_bar_draw(cr, mx, y + (STAT_ROW_H - METER_H) / 2.0, METER_W, METER_H,
                   total_gb > 0.0 ? used_gb / total_gb : 0.0,
                   METER_SEGMENTS, gradient, COLORS.surface0);
    char used_s[24], value[64];
    format_gb(used_s, sizeof(used_s), used_gb);
    snprintf(value, sizeof(value), "%s / %.1f GB", used_s, total_gb);
    module_box_draw_text_left(cr, mx + METER_W + POPUP_COL_GAP, y, STAT_ROW_H, value, FONT.label, false, COLORS.text);
    char pct[16];
    snprintf(pct, sizeof(pct), "%d%%", percent);
    module_box_draw_text_right(cr, x + w, y, STAT_ROW_H, pct, FONT.label, false, COLORS.subtext0);
}

static double paint_rows(cairo_t *cr, double x, double y, double w) {
    double h = STAT_ROW_H * (2 + text_row_count());
    if (!cr) {
        return h;
    }
    meter_row(cr, x, y, w, "RAM", sysmon_mem_used_gb(), sysmon_mem_total_gb(), sysmon_mem_percent());
    if (sysmon_swap_total_gb() > 0.0) {
        meter_row(cr, x, y + STAT_ROW_H, w, "SWAP", sysmon_swap_used_gb(), sysmon_swap_total_gb(),
                  sysmon_swap_percent());
    } else {
        module_box_draw_text_left(cr, x, y + STAT_ROW_H, STAT_ROW_H, "SWAP", FONT.xs, true, COLORS.overlay0);
        module_box_draw_text_left(cr, x + label_col_w() + POPUP_COL_GAP, y + STAT_ROW_H, STAT_ROW_H, "none",
                                  FONT.label, false,
                                  COLORS.subtext0);
    }

    double value_x = x + label_col_w() + POPUP_COL_GAP;
    double ry = y + STAT_ROW_H * 2.0;
    char text[128];
    if (sysmon_swap_is_zram()) {
        zram_text(text, sizeof(text));
        module_box_draw_text_left(cr, x, ry, STAT_ROW_H, "ZRAM", FONT.xs, true, COLORS.overlay0);
        module_box_draw_text_left(cr, value_x, ry, STAT_ROW_H, text, FONT.label, false, COLORS.text);
        ry += STAT_ROW_H;
    }
    avail_text(text, sizeof(text));
    module_box_draw_text_left(cr, x, ry, STAT_ROW_H, "AVAIL", FONT.xs, true, COLORS.overlay0);
    module_box_draw_text_left(cr, value_x, ry, STAT_ROW_H, text, FONT.label, false, COLORS.text);
    ry += STAT_ROW_H;
    if (sysmon_mem_psi_available()) {
        psi_text(text, sizeof(text));
        module_box_draw_text_left(cr, x, ry, STAT_ROW_H, "PRESS", FONT.xs, true, COLORS.overlay0);
        module_box_draw_text_left(cr, value_x, ry, STAT_ROW_H, text, FONT.label, false, COLORS.text);
    }
    return h;
}

// "TOP APPS": always SYSMON_MEM_MAX_PROCS rows tall (fixed size).
static double paint_apps(cairo_t *cr, double x, double y, double w) {
    double h = STAT_ROW_H * (1 + SYSMON_MEM_MAX_PROCS);
    if (!cr) {
        return h;
    }
    module_box_draw_text_left(cr, x, y, STAT_ROW_H, "TOP APPS", FONT.xs, true, COLORS.overlay0);
    int count = sysmon_mem_process_count();
    double biggest = sysmon_mem_process_gb(0);
    for (int i = 0; i < count; i++) {
        double ry = y + STAT_ROW_H * (i + 1);
        double fraction = biggest > 0.0 ? sysmon_mem_process_gb(i) / biggest : 0.0;
        char size[24];
        snprintf(size, sizeof(size), "%.2f GB", sysmon_mem_process_gb(i));
        stat_draw_app_row(cr, x, ry, w, sysmon_mem_process_name(i), fraction, COLORS.yellow, size);
    }
    return h;
}

static double paint_content(cairo_t *cr, double w) {
    double x = POPUP_OUTER_MARGIN, y = POPUP_OUTER_MARGIN;
    y += paint_header(cr, x, y, w);
    y += POPUP_SECTION_GAP;
    y += paint_graph_card(cr, x, y, w);
    y += stat_graph_axis(cr, x, y, w, stat_graph_cells(w, SYSMON_MEM_HISTORY_LEN)) + POPUP_SECTION_GAP;
    y += paint_split(cr, x, y, w);
    y += POPUP_SECTION_GAP;
    y += paint_rows(cr, x, y, w);
    y += POPUP_SECTION_GAP;
    y += paint_apps(cr, x, y, w);
    return y + POPUP_OUTER_MARGIN;
}

static double content_width(void) {
    double w = braille_graph_width(STAT_GRAPH_MIN_CELLS_W) + STAT_CARD_PAD * 2.0;

    char icon[5], rest[96];
    glyph_utf8(GLYPH_ICON_MEM, icon);
    header_text(rest, sizeof(rest));
    // Sized for the widest pill word.
    PopupHeader header = {.icon = icon, .title = "Memory", .summary = rest, .pill = "thrashing"};
    double header_w = popup_header_width(&header);
    if (header_w > w) {
        w = header_w;
    }

    // Values change while open; measure each text row with headroom.
    char zram[128];
    zram_text(zram, sizeof(zram));
    const char *values[] = {
        "00.0 GB · shared 000 MB · dirty 0000 MB",
        "00.00 / 00.00 / 00.00 %  (10s / 60s / 5m)",
        sysmon_swap_is_zram() ? zram : "",
        "zstd · 000 MB -> 000 MB (0.0x) · uses 000 MB",
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        double rw = label_col_w() + POPUP_COL_GAP + module_box_text_width(values[i], FONT.label, false);
        if (rw > w) {
            w = rw;
        }
    }
    double meter_w = label_col_w() + POPUP_COL_GAP + METER_W + POPUP_COL_GAP + module_box_text_width("000 MB / 00.0 GB",
                                                                                         FONT.label, false) +
                     POPUP_COL_GAP + module_box_text_width("100%", FONT.label, false);
    if (meter_w > w) {
        w = meter_w;
    }
    double apps_w = stat_app_name_col_w() + STAT_APP_BAR_W + POPUP_COL_GAP + module_box_text_width("00.00 GB",
                                                                                                   FONT.label, false);
    return apps_w > w ? apps_w : w;
}

static void draw_popup(cairo_t *cr, void *user_data) {
    (void)user_data;
    popup_draw_frame(cr, g_popup.popup_width, g_popup.popup_height, COLORS.yellow);
    paint_content(cr, g_popup.popup_width - POPUP_OUTER_MARGIN * 2.0);
}

void mem_popup_open(Output *output, double box_x, double box_y, double box_w, double box_h) {
    mem_popup_close();
    cpu_popup_close(); // all three sysmon dropdowns open at the same spot
    gpu_popup_close();

    // A snapshot, so the figures are real right away (no second scan, unlike
    // the CPU's process list).
    sysmon_mem_refresh_details();

    g_popup.host.output = output;
    double content_w = content_width();
    g_popup.popup_width = content_w + POPUP_OUTER_MARGIN * 2.0;
    g_popup.popup_height = paint_content(NULL, content_w);

    // Under its box, opening rightward from the box's left edge (bar/popup_host.c).
    if (!popup_host_open(&g_popup.host, output, POPUP_ANCHOR_LEFT, box_x, box_y, box_w, box_h,
                         (int)g_popup.popup_width, (int)g_popup.popup_height)) {
        return;
    }
    popup_surface_render_now(g_popup.host.popup, draw_popup, NULL);
}

void mem_popup_refresh_if_open(void) {
    if (!g_popup.host.open) {
        return;
    }
    sysmon_mem_refresh_details();
    popup_surface_render_now(g_popup.host.popup, draw_popup, NULL);
}

void mem_popup_close(void) {
    if (!g_popup.host.open) {
        return;
    }
    popup_host_close(&g_popup.host);
}

bool mem_popup_is_menu_surface(struct wl_surface *surface) {
    return popup_host_is_menu_surface(&g_popup.host, surface);
}

bool mem_popup_is_catcher_surface(struct wl_surface *surface) {
    return popup_host_is_catcher_surface(&g_popup.host, surface);
}
