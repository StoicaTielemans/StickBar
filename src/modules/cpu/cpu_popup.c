#include "modules/cpu/cpu_popup.h"

#include <stdio.h>
#include <string.h>

#include "bar/popup_host.h"
#include "bar/popup_surface.h"
#include "config/glyphs.h"
#include "modules/gpu/gpu_popup.h"
#include "modules/mem/mem_popup.h"
#include "render/braille_graph.h"
#include "render/meter_bar.h"
#include "render/module_box.h"
#include "render/popup_chrome.h"
#include "render/popup_style.h"
#include "render/stat_card.h"
#include "sysmon/sysmon_client.h"
#include "util/file.h"

// Layout tuning, local to this popup (see CLAUDE.md "Conventions"). Section
// rhythm and graph card match modules/gpu/gpu_popup.c on purpose, so the two
// dropdowns read as a pair.
#define USAGE_CELLS_H 4
#define TEMP_CELLS_H 2
#define CORE_ROW_H 15.0
#define CELL_GAP 6.0       // between the parts of one core cell
#define CORE_COLUMN_GAP 18.0
#define CORE_METER_W 60.0
#define CORE_METER_H 6.0
#define CORE_METER_SEGMENTS 6

#define TEMP_GRAPH_MIN_C 30.0
#define TEMP_GRAPH_MAX_C 90.0

typedef struct {
    PopupHost host; // the dropdown surface, its placement and click-catchers
    bool first_tick_done; // the ~1s-after-open process sample has run
    double opened_at;     // CLOCK_MONOTONIC seconds
    double popup_width, popup_height;

} CpuPopupState;

static CpuPopupState g_popup;

// Shortest window the first process figure is measured over: the clock
// timer fires on whole seconds, so the first tick after opening can land
// only milliseconds later -- far too short to measure anything meaningful.
#define MIN_FIRST_SAMPLE_S 0.5

// Green -> yellow -> red; a function since COLORS isn't a compile-time
// constant (see docs/modules/power-menu.md).
static void heat_gradient(Color out[3]) {
    out[0] = COLORS.green;
    out[1] = COLORS.yellow;
    out[2] = COLORS.red;
}

// "12th Gen Intel(R) Core(TM) i5-12600K" -> "i5-12600K";
// "AMD Ryzen 7 5800X 8-Core Processor" -> "AMD Ryzen 7 5800X".
static void short_model(char *out, size_t out_size) {
    const char *model = sysmon_cpu_model();
    const char *core_tm = strstr(model, "Core(TM) ");
    snprintf(out, out_size, "%s", core_tm ? core_tm + strlen("Core(TM) ") : (model[0] ? model : "CPU"));
    char *cut = strstr(out, " Processor");
    if (cut) {
        *cut = '\0';
    }
    char *n_core = strstr(out, "-Core");
    if (n_core) {
        // Drop the " 8-Core" word: back up to the space before its number.
        while (n_core > out && n_core[-1] != ' ') {
            n_core--;
        }
        if (n_core > out) {
            n_core[-1] = '\0';
        }
    }
}

static void counts_text(char *out, size_t out_size) {
    if (sysmon_cpu_physical_cores() > 0) {
        snprintf(out, out_size, "  ·  %d cores / %d threads", sysmon_cpu_physical_cores(), sysmon_cpu_core_count());
    } else {
        snprintf(out, out_size, "  ·  %d threads", sysmon_cpu_core_count());
    }
}

// ---- Core grid ----------------------------------------------------------------

// One core cell: "12 ▰▰▱▱▱▱  34% 4.4G". Widths fixed from sample strings
// (the font is monospace), so cells line up and never shift as values change.
static double core_index_w(void) { return module_box_text_width("00", FONT.sm, false); }
static double core_percent_w(void) { return module_box_text_width("100%", FONT.sm, false); }
static double core_ghz_w(void) { return module_box_text_width("0.0G", FONT.xs, false); }
static double core_cell_w(void) {
    return core_index_w() + CELL_GAP + CORE_METER_W + CELL_GAP + core_percent_w() + CELL_GAP + core_ghz_w();
}

static void draw_core_cell(cairo_t *cr, double x, double y, int core) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", core);
    module_box_draw_text_right(cr, x + core_index_w(), y, CORE_ROW_H, buf, FONT.sm, false, COLORS.overlay0);
    double cx = x + core_index_w() + CELL_GAP;
    Color gradient[3];
    heat_gradient(gradient);
    double pct = sysmon_cpu_core_percent(core);
    meter_bar_draw(cr, cx, y + (CORE_ROW_H - CORE_METER_H) / 2.0, CORE_METER_W, CORE_METER_H, pct / 100.0,
                   CORE_METER_SEGMENTS, gradient, COLORS.surface0);
    cx += CORE_METER_W + CELL_GAP;
    snprintf(buf, sizeof(buf), "%.0f%%", pct);
    module_box_draw_text_right(cr, cx + core_percent_w(), y, CORE_ROW_H, buf, FONT.sm, false, COLORS.text);
    cx += core_percent_w() + CELL_GAP;
    double ghz = sysmon_cpu_core_ghz(core);
    if (ghz > 0.0) {
        snprintf(buf, sizeof(buf), "%.1fG", ghz);
        module_box_draw_text_left(cr, cx, y, CORE_ROW_H, buf, FONT.xs, false, COLORS.overlay0);
    }
}

// One titled group of threads (P-cores, E-cores, or all), two per row.
// `efficient` picks the group on hybrid chips; ignored otherwise.
static double paint_core_group(cairo_t *cr, double x, double y, const char *title, bool hybrid, bool efficient) {
    int cores[256];
    int n = 0;
    for (int i = 0; i < sysmon_cpu_core_count() && n < 256; i++) {
        if (!hybrid || sysmon_cpu_core_is_efficient(i) == efficient) {
            cores[n++] = i;
        }
    }
    if (n == 0) {
        return 0.0;
    }
    int rows = (n + 1) / 2;
    double h = STAT_ROW_H + rows * CORE_ROW_H;
    if (!cr) {
        return h;
    }
    module_box_draw_text_left(cr, x, y, STAT_ROW_H, title, FONT.xs, true, COLORS.overlay0);
    // Column-major (0..5 down the left, 6..11 down the right), like btop.
    for (int i = 0; i < n; i++) {
        int col = i / rows, row = i % rows;
        draw_core_cell(cr, x + col * (core_cell_w() + CORE_COLUMN_GAP), y + STAT_ROW_H + row * CORE_ROW_H, cores[i]);
    }
    return h;
}

static double paint_cores(cairo_t *cr, double x, double y) {
    if (!sysmon_cpu_hybrid()) {
        return paint_core_group(cr, x, y, "CORES", false, false);
    }
    double h = paint_core_group(cr, x, y, "P-CORES", true, false);
    h += 4.0;
    h += paint_core_group(cr, x, y + h, "E-CORES", true, true);
    return h;
}

// ---- Painting ----------------------------------------------------------------
// Every paint_* takes a NULL `cr` to mean "measure only" and returns its
// height, so the size computed at open and the drawn layout can't drift.

static double paint_header(cairo_t *cr, double x, double y, double w) {
    if (!cr) {
        return POPUP_HEADER_H;
    }
    char icon[5], model[96], counts[64];
    glyph_utf8(GLYPH_ICON_CPU, icon);
    short_model(model, sizeof(model));
    counts_text(counts, sizeof(counts));
    // Load pill: the same three buckets as the GPU's P-state pill.
    double usage = sysmon_cpu_percent();
    PopupHeader header = {
        .icon = icon, .icon_color = COLORS.peach, .title = model, .summary = counts,
        .pill = usage < 15.0 ? "idle" : usage < 60.0 ? "active" : "busy",
        .pill_fill = usage < 15.0 ? COLORS.green : usage < 60.0 ? COLORS.yellow : COLORS.peach,
    };
    popup_draw_header(cr, x, y, w, &header);
    return POPUP_HEADER_H;
}

static double paint_graph_card(cairo_t *cr, double x, double y, double w) {
    int cells = stat_graph_cells(w, SYSMON_CPU_HISTORY_LEN);
    double graph_w = braille_graph_width(cells);
    double usage_h = braille_graph_height(USAGE_CELLS_H);
    double temp_h = braille_graph_height(TEMP_CELLS_H);
    double h = STAT_CARD_PAD * 2.0 + (STAT_GRAPH_LABEL_H + STAT_GRAPH_LABEL_GAP) * 2.0 + usage_h + STAT_GRAPH_SPLIT_GAP +
               temp_h;
    if (!cr) {
        return h;
    }
    popup_draw_card(cr, x, y, w, h, false, false, COLORS.surface1);

    // Only the samples the graph has room for (and so the peaks over them).
    int n;
    int first = stat_history_window(sysmon_cpu_history_len(), cells, &n);
    double usage[SYSMON_CPU_HISTORY_LEN], temp[SYSMON_CPU_HISTORY_LEN];
    double usage_peak = 0.0, temp_peak = 0.0;
    for (int i = 0; i < n; i++) {
        usage[i] = sysmon_cpu_util_history(first + i);
        double t = sysmon_cpu_temp_history(first + i);
        // Zero of the graph is TEMP_GRAPH_MIN_C (see gpu_popup.c).
        temp[i] = t > TEMP_GRAPH_MIN_C ? t - TEMP_GRAPH_MIN_C : 0.0;
        if (usage[i] > usage_peak) {
            usage_peak = usage[i];
        }
        if (t > temp_peak) {
            temp_peak = t;
        }
    }

    double inner_x = x + STAT_CARD_PAD, inner_w = w - STAT_CARD_PAD * 2.0;
    double graph_x = x + (w - graph_w) / 2.0;
    double cy = y + STAT_CARD_PAD;
    char left[48], right[48];

    snprintf(left, sizeof(left), "usage %.0f%%", sysmon_cpu_percent());
    snprintf(right, sizeof(right), "peak %.0f%%", usage_peak);
    stat_graph_label_row(cr, inner_x, cy, inner_w, left, COLORS.peach, right);
    cy += STAT_GRAPH_LABEL_H + STAT_GRAPH_LABEL_GAP;
    Color usage_gradient[3];
    heat_gradient(usage_gradient);
    braille_graph_draw(cr, graph_x, cy, cells, USAGE_CELLS_H, usage, n, 100.0, true, usage_gradient);
    cy += usage_h + STAT_GRAPH_SPLIT_GAP;

    snprintf(left, sizeof(left), "temp %d°C", sysmon_cpu_temp_c());
    snprintf(right, sizeof(right), "max %.0f°C", temp_peak);
    stat_graph_label_row(cr, inner_x, cy, inner_w, left, COLORS.peach, right);
    cy += STAT_GRAPH_LABEL_H + STAT_GRAPH_LABEL_GAP;
    const Color temp_gradient[3] = {COLORS.peach, COLORS.maroon, COLORS.red};
    braille_graph_draw(cr, graph_x, cy, cells, TEMP_CELLS_H, temp, n, TEMP_GRAPH_MAX_C - TEMP_GRAPH_MIN_C,
                       true, temp_gradient);
    return h;
}

static const char *ROW_LABELS[] = {"LOAD", "CLOCK", "UPTIME"};

static double label_col_w(void) {
    return stat_label_col_w(ROW_LABELS, sizeof(ROW_LABELS) / sizeof(ROW_LABELS[0]));
}

static void row_texts(char load[96], char clock[64], char uptime[48]) {
    snprintf(load, 96, "%s · %d running / %d tasks", sysmon_cpu_load_avg(), sysmon_cpu_tasks_running(),
             sysmon_cpu_tasks_total());
    double sum = 0.0;
    int n = 0;
    for (int i = 0; i < sysmon_cpu_core_count(); i++) {
        if (sysmon_cpu_core_ghz(i) > 0.0) {
            sum += sysmon_cpu_core_ghz(i);
            n++;
        }
    }
    // Fall back to /proc/cpuinfo's average (what the old text dropdown
    // showed) if cpufreq isn't exposed.
    double avg = n > 0 ? sum / n : sysmon_cpu_freq_ghz();
    if (sysmon_cpu_max_ghz() > 0.0) {
        snprintf(clock, 64, "avg %.1f GHz · max %.1f GHz", avg, sysmon_cpu_max_ghz());
    } else {
        snprintf(clock, 64, "avg %.1f GHz", avg);
    }
    long up = (long)sysmon_cpu_uptime_s();
    long days = up / 86400, hours = (up % 86400) / 3600, mins = (up % 3600) / 60;
    if (days > 0) {
        snprintf(uptime, 48, "%ldd %ldh %ldm", days, hours, mins);
    } else {
        snprintf(uptime, 48, "%ldh %ldm", hours, mins);
    }
}

static double paint_rows(cairo_t *cr, double x, double y) {
    if (!cr) {
        return STAT_ROW_H * 3.0;
    }
    double value_x = x + label_col_w() + POPUP_COL_GAP;
    char load[96], clock[64], uptime[48];
    row_texts(load, clock, uptime);
    const char *values[] = {load, clock, uptime};
    for (int i = 0; i < 3; i++) {
        stat_draw_kv_row(cr, x, value_x, y + STAT_ROW_H * i, ROW_LABELS[i], values[i]);
    }
    return STAT_ROW_H * 3.0;
}

// "TOP PROCESSES": always SYSMON_CPU_MAX_PROCS rows tall (fixed size).
static double paint_apps(cairo_t *cr, double x, double y, double w) {
    double h = STAT_ROW_H * (1 + SYSMON_CPU_MAX_PROCS);
    if (!cr) {
        return h;
    }
    module_box_draw_text_left(cr, x, y, STAT_ROW_H, "TOP PROCESSES", FONT.xs, true, COLORS.overlay0);
    if (!sysmon_cpu_process_ready()) {
        module_box_draw_text_left(cr, x, y + STAT_ROW_H, STAT_ROW_H, "measuring...", FONT.label, false,
                                  COLORS.subtext0);
        return h;
    }
    int count = sysmon_cpu_process_count();
    if (count == 0) {
        module_box_draw_text_left(cr, x, y + STAT_ROW_H, STAT_ROW_H, "nothing busy", FONT.label, false,
                                  COLORS.subtext0);
        return h;
    }
    double biggest = sysmon_cpu_process_percent(0);
    for (int i = 0; i < count; i++) {
        double ry = y + STAT_ROW_H * (i + 1);
        double fraction = biggest > 0.0 ? sysmon_cpu_process_percent(i) / biggest : 0.0;
        char pct[32];
        snprintf(pct, sizeof(pct), "%.1f%%", sysmon_cpu_process_percent(i));
        stat_draw_app_row(cr, x, ry, w, sysmon_cpu_process_name(i), fraction, COLORS.peach, pct);
    }
    return h;
}

static double paint_content(cairo_t *cr, double w) {
    double x = POPUP_OUTER_MARGIN, y = POPUP_OUTER_MARGIN;
    y += paint_header(cr, x, y, w);
    y += POPUP_SECTION_GAP;
    y += paint_graph_card(cr, x, y, w);
    y += stat_graph_axis(cr, x, y, w, stat_graph_cells(w, SYSMON_CPU_HISTORY_LEN)) + POPUP_SECTION_GAP;
    y += paint_cores(cr, x, y);
    y += POPUP_SECTION_GAP;
    y += paint_rows(cr, x, y);
    y += POPUP_SECTION_GAP;
    y += paint_apps(cr, x, y, w);
    return y + POPUP_OUTER_MARGIN;
}

static double content_width(void) {
    double w = braille_graph_width(STAT_GRAPH_MIN_CELLS_W) + STAT_CARD_PAD * 2.0;
    double cores_w = core_cell_w() * 2.0 + CORE_COLUMN_GAP;
    if (cores_w > w) {
        w = cores_w;
    }

    char icon[5], model[96], counts[64];
    glyph_utf8(GLYPH_ICON_CPU, icon);
    short_model(model, sizeof(model));
    counts_text(counts, sizeof(counts));
    // Sized for the widest pill word.
    PopupHeader header = {.icon = icon, .title = model, .summary = counts, .pill = "active"};
    double header_w = popup_header_width(&header);
    if (header_w > w) {
        w = header_w;
    }

    char load[96], clock[64], uptime[48];
    row_texts(load, clock, uptime);
    // Task counts can grow while open; measure with some headroom.
    const char *values[] = {load, clock, "00.00 00.00 00.00 · 00 running / 00000 tasks"};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        double rw = label_col_w() + POPUP_COL_GAP + module_box_text_width(values[i], FONT.label, false);
        if (rw > w) {
            w = rw;
        }
    }
    double apps_w = stat_app_name_col_w() + STAT_APP_BAR_W + POPUP_COL_GAP + module_box_text_width("100.0%",
                                                                                                   FONT.label, false);
    return apps_w > w ? apps_w : w;
}

static void draw_popup(cairo_t *cr, void *user_data) {
    (void)user_data;
    popup_draw_frame(cr, g_popup.popup_width, g_popup.popup_height, COLORS.peach);
    paint_content(cr, g_popup.popup_width - POPUP_OUTER_MARGIN * 2.0);
}

void cpu_popup_open(Output *output, double box_x, double box_y, double box_w, double box_h) {
    cpu_popup_close();
    gpu_popup_close();         // all three sysmon dropdowns open at the same spot
    mem_popup_close();

    // Start a fresh process measurement: this scan only primes it, the one
    // cpu_popup_tick() runs ~1s from now produces the first real figures.
    sysmon_cpu_reset_process_sample();
    sysmon_cpu_refresh_details();

    g_popup.host.output = output;
    g_popup.first_tick_done = false;
    g_popup.opened_at = monotonic_seconds();
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

void cpu_popup_tick(void) {
    // Once only: the first clock tick at least MIN_FIRST_SAMPLE_S after
    // opening turns the priming scan into real process figures, instead of
    // "measuring..." for up to 5s until the next sysmon poll.
    if (!g_popup.host.open || g_popup.first_tick_done) {
        return;
    }
    if (monotonic_seconds() - g_popup.opened_at < MIN_FIRST_SAMPLE_S) {
        return;
    }
    sysmon_cpu_refresh_details();
    g_popup.first_tick_done = true;
    popup_surface_render_now(g_popup.host.popup, draw_popup, NULL);
}

void cpu_popup_refresh_if_open(void) {
    if (!g_popup.host.open) {
        return;
    }
    sysmon_cpu_refresh_details();
    popup_surface_render_now(g_popup.host.popup, draw_popup, NULL);
}

void cpu_popup_close(void) {
    if (!g_popup.host.open) {
        return;
    }
    popup_host_close(&g_popup.host);
}

bool cpu_popup_is_menu_surface(struct wl_surface *surface) {
    return popup_host_is_menu_surface(&g_popup.host, surface);
}

bool cpu_popup_is_catcher_surface(struct wl_surface *surface) {
    return popup_host_is_catcher_surface(&g_popup.host, surface);
}
