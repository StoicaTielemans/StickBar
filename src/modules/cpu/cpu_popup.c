#include "modules/cpu/cpu_popup.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bar/popup_surface.h"
#include "config/glyphs.h"
#include "modules/gpu/gpu_popup.h"
#include "modules/mem/mem_popup.h"
#include "render/braille_graph.h"
#include "render/meter_bar.h"
#include "render/module_box.h"
#include "sysmon/sysmon_client.h"

// Sized generously (this machine has 2 monitors), matching the same
// MAX_TRACKED_OUTPUTS convention used by every other popup in this project.
#define MAX_TRACKED_OUTPUTS 8

// Layout tuning, local to this popup (see CLAUDE.md "Conventions"). Section
// rhythm and graph card match modules/gpu/gpu_popup.c on purpose, so the two
// dropdowns read as a pair.
#define OUTER_MARGIN 12.0
#define SECTION_GAP 10.0
#define HEADER_H 22.0
#define PILL_H 16.0
#define PILL_PAD_X 7.0
#define CARD_PAD 8.0
#define GRAPH_LABEL_H 14.0
#define GRAPH_LABEL_GAP 3.0
#define GRAPH_SPLIT_GAP 8.0
#define GRAPH_MIN_CELLS_W 30 // 5 minutes of 5s samples, one per dot column
#define USAGE_CELLS_H 4
#define TEMP_CELLS_H 2
#define AXIS_H 12.0
#define ROW_H 17.0
#define CORE_ROW_H 15.0
#define COL_GAP 10.0
#define CELL_GAP 6.0       // between the parts of one core cell
#define CORE_COLUMN_GAP 18.0
#define CORE_METER_W 60.0
#define CORE_METER_H 6.0
#define CORE_METER_SEGMENTS 6
#define APP_BAR_W 70.0
#define APP_BAR_H 5.0

#define TEMP_GRAPH_MIN_C 30.0
#define TEMP_GRAPH_MAX_C 90.0

typedef struct {
    bool open;
    bool first_tick_done; // the ~1s-after-open process sample has run
    double opened_at;     // CLOCK_MONOTONIC seconds
    Output *output;
    PopupSurface *popup;
    double popup_width, popup_height;

    PopupSurface *catchers[MAX_TRACKED_OUTPUTS];
    struct wl_surface *catcher_surfaces[MAX_TRACKED_OUTPUTS];
    int catcher_count;
} CpuPopupState;

static CpuPopupState g_popup;

// Shortest window the first process figure is measured over: the clock
// timer fires on whole seconds, so the first tick after opening can land
// only milliseconds later -- far too short to measure anything meaningful.
#define MIN_FIRST_SAMPLE_S 0.5

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
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

// The graph fills its card: as many cells as fit (one 5s sample per dot
// column), up to the whole history. GRAPH_MIN_CELLS_W (5 minutes) only sets
// the card's minimum width; a wider card shows a longer stretch of history.
static int graph_cells(double w) {
    int cells = braille_graph_cells_for_width(w - CARD_PAD * 2.0);
    return cells < SYSMON_CPU_HISTORY_LEN / 2 ? cells : SYSMON_CPU_HISTORY_LEN / 2;
}

// Green -> yellow -> red; a function since COLORS isn't a compile-time
// constant (see CLAUDE.md "Power menu").
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
static double core_index_w(void) { return text_width("00", FONT.sm, false); }
static double core_percent_w(void) { return text_width("100%", FONT.sm, false); }
static double core_ghz_w(void) { return text_width("0.0G", FONT.xs, false); }
static double core_cell_w(void) {
    return core_index_w() + CELL_GAP + CORE_METER_W + CELL_GAP + core_percent_w() + CELL_GAP + core_ghz_w();
}

static void draw_core_cell(cairo_t *cr, double x, double y, int core) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", core);
    draw_text_right(cr, x + core_index_w(), y, CORE_ROW_H, buf, FONT.sm, false, COLORS.overlay0);
    double cx = x + core_index_w() + CELL_GAP;
    Color gradient[3];
    heat_gradient(gradient);
    double pct = sysmon_cpu_core_percent(core);
    meter_bar_draw(cr, cx, y + (CORE_ROW_H - CORE_METER_H) / 2.0, CORE_METER_W, CORE_METER_H, pct / 100.0,
                   CORE_METER_SEGMENTS, gradient, COLORS.surface0);
    cx += CORE_METER_W + CELL_GAP;
    snprintf(buf, sizeof(buf), "%.0f%%", pct);
    draw_text_right(cr, cx + core_percent_w(), y, CORE_ROW_H, buf, FONT.sm, false, COLORS.text);
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
    double h = ROW_H + rows * CORE_ROW_H;
    if (!cr) {
        return h;
    }
    module_box_draw_text_left(cr, x, y, ROW_H, title, FONT.xs, true, COLORS.overlay0);
    // Column-major (0..5 down the left, 6..11 down the right), like btop.
    for (int i = 0; i < n; i++) {
        int col = i / rows, row = i % rows;
        draw_core_cell(cr, x + col * (core_cell_w() + CORE_COLUMN_GAP), y + ROW_H + row * CORE_ROW_H, cores[i]);
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
        return HEADER_H;
    }
    char icon[5], model[96], counts[64];
    glyph_utf8(GLYPH_ICON_CPU, icon);
    short_model(model, sizeof(model));
    counts_text(counts, sizeof(counts));
    module_box_draw_text_left(cr, x, y, HEADER_H, icon, FONT.heading, false, COLORS.peach);
    double tx = x + text_width(icon, FONT.heading, false) + 8.0;
    module_box_draw_text_left(cr, tx, y, HEADER_H, model, FONT.md, true, COLORS.text);
    tx += text_width(model, FONT.md, true);
    module_box_draw_text_left(cr, tx, y, HEADER_H, counts, FONT.sm, false, COLORS.subtext0);

    // Load pill: the same three buckets as the GPU's P-state pill.
    double usage = sysmon_cpu_percent();
    Color fill = usage < 15.0 ? COLORS.green : usage < 60.0 ? COLORS.yellow : COLORS.peach;
    const char *word = usage < 15.0 ? "idle" : usage < 60.0 ? "active" : "busy";
    double pill_w = text_width(word, FONT.xs, true) + PILL_PAD_X * 2.0;
    double pill_x = x + w - pill_w;
    double pill_y = y + (HEADER_H - PILL_H) / 2.0;
    module_box_draw_rect(cr, pill_x, pill_y, pill_w, PILL_H, fill, COLORS.mantle, 0.0);
    module_box_draw_text_centered(cr, pill_x, pill_y, pill_w, PILL_H, word, FONT.xs, true, COLORS.mantle);
    return HEADER_H;
}

static void graph_label_row(cairo_t *cr, double x, double y, double w, const char *left, Color left_color,
                            const char *right) {
    module_box_draw_text_left(cr, x, y, GRAPH_LABEL_H, left, FONT.sm, true, left_color);
    draw_text_right(cr, x + w, y, GRAPH_LABEL_H, right, FONT.xs, false, COLORS.overlay0);
}

static double paint_graph_card(cairo_t *cr, double x, double y, double w) {
    int cells = graph_cells(w);
    double graph_w = braille_graph_width(cells);
    double usage_h = braille_graph_height(USAGE_CELLS_H);
    double temp_h = braille_graph_height(TEMP_CELLS_H);
    double h = CARD_PAD * 2.0 + (GRAPH_LABEL_H + GRAPH_LABEL_GAP) * 2.0 + usage_h + GRAPH_SPLIT_GAP + temp_h;
    if (!cr) {
        return h;
    }
    module_box_draw_rect(cr, x, y, w, h, COLORS.base, COLORS.surface1, 1.0);

    // Only the samples the graph has room for (and so the peaks over them).
    int first = sysmon_cpu_history_len() - cells * 2;
    if (first < 0) {
        first = 0;
    }
    int n = sysmon_cpu_history_len() - first;
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

    double inner_x = x + CARD_PAD, inner_w = w - CARD_PAD * 2.0;
    double graph_x = x + (w - graph_w) / 2.0;
    double cy = y + CARD_PAD;
    char left[48], right[48];

    snprintf(left, sizeof(left), "usage %.0f%%", sysmon_cpu_percent());
    snprintf(right, sizeof(right), "peak %.0f%%", usage_peak);
    graph_label_row(cr, inner_x, cy, inner_w, left, COLORS.peach, right);
    cy += GRAPH_LABEL_H + GRAPH_LABEL_GAP;
    Color usage_gradient[3];
    heat_gradient(usage_gradient);
    braille_graph_draw(cr, graph_x, cy, cells, USAGE_CELLS_H, usage, n, 100.0, true, usage_gradient);
    cy += usage_h + GRAPH_SPLIT_GAP;

    snprintf(left, sizeof(left), "temp %d°C", sysmon_cpu_temp_c());
    snprintf(right, sizeof(right), "max %.0f°C", temp_peak);
    graph_label_row(cr, inner_x, cy, inner_w, left, COLORS.peach, right);
    cy += GRAPH_LABEL_H + GRAPH_LABEL_GAP;
    const Color temp_gradient[3] = {COLORS.peach, COLORS.maroon, COLORS.red};
    braille_graph_draw(cr, graph_x, cy, cells, TEMP_CELLS_H, temp, n, TEMP_GRAPH_MAX_C - TEMP_GRAPH_MIN_C,
                       true, temp_gradient);
    return h;
}

static const char *ROW_LABELS[] = {"LOAD", "CLOCK", "UPTIME"};

static double label_col_w(void) {
    double w = 0.0;
    for (size_t i = 0; i < sizeof(ROW_LABELS) / sizeof(ROW_LABELS[0]); i++) {
        double lw = text_width(ROW_LABELS[i], FONT.xs, true);
        if (lw > w) {
            w = lw;
        }
    }
    return w;
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
        return ROW_H * 3.0;
    }
    double value_x = x + label_col_w() + COL_GAP;
    char load[96], clock[64], uptime[48];
    row_texts(load, clock, uptime);
    const char *values[] = {load, clock, uptime};
    for (int i = 0; i < 3; i++) {
        module_box_draw_text_left(cr, x, y + ROW_H * i, ROW_H, ROW_LABELS[i], FONT.xs, true, COLORS.overlay0);
        module_box_draw_text_left(cr, value_x, y + ROW_H * i, ROW_H, values[i], FONT.label, false, COLORS.text);
    }
    return ROW_H * 3.0;
}

// /proc/PID/comm names are at most 15 characters and the font is monospace
// (see gpu_popup.c's app_name_col_w()).
static double app_name_col_w(void) {
    return text_width("000000000000000", FONT.label, false) + COL_GAP;
}

// "TOP PROCESSES": always SYSMON_CPU_MAX_PROCS rows tall (fixed size).
static double paint_apps(cairo_t *cr, double x, double y, double w) {
    double h = ROW_H * (1 + SYSMON_CPU_MAX_PROCS);
    if (!cr) {
        return h;
    }
    module_box_draw_text_left(cr, x, y, ROW_H, "TOP PROCESSES", FONT.xs, true, COLORS.overlay0);
    if (!sysmon_cpu_process_ready()) {
        module_box_draw_text_left(cr, x, y + ROW_H, ROW_H, "measuring...", FONT.label, false, COLORS.subtext0);
        return h;
    }
    int count = sysmon_cpu_process_count();
    if (count == 0) {
        module_box_draw_text_left(cr, x, y + ROW_H, ROW_H, "nothing busy", FONT.label, false, COLORS.subtext0);
        return h;
    }
    double biggest = sysmon_cpu_process_percent(0);
    const Color app_gradient[3] = {COLORS.peach, COLORS.peach, COLORS.peach};
    for (int i = 0; i < count; i++) {
        double ry = y + ROW_H * (i + 1);
        module_box_draw_text_left(cr, x, ry, ROW_H, sysmon_cpu_process_name(i), FONT.label, false, COLORS.text);
        double fraction = biggest > 0.0 ? sysmon_cpu_process_percent(i) / biggest : 0.0;
        meter_bar_draw(cr, x + app_name_col_w(), ry + (ROW_H - APP_BAR_H) / 2.0, APP_BAR_W, APP_BAR_H, fraction, 7,
                       app_gradient, COLORS.surface0);
        char pct[32];
        snprintf(pct, sizeof(pct), "%.1f%%", sysmon_cpu_process_percent(i));
        draw_text_right(cr, x + w, ry, ROW_H, pct, FONT.label, false, COLORS.subtext0);
    }
    return h;
}

static double paint_content(cairo_t *cr, double w) {
    double x = OUTER_MARGIN, y = OUTER_MARGIN;
    y += paint_header(cr, x, y, w);
    y += SECTION_GAP;
    y += paint_graph_card(cr, x, y, w);
    if (cr) {
        char span[24];
        braille_graph_span_label(span, sizeof(span), graph_cells(w), 5.0);
        module_box_draw_text_left(cr, x, y + 2.0, AXIS_H, span, FONT.xs, false, COLORS.overlay0);
        draw_text_right(cr, x + w, y + 2.0, AXIS_H, "now", FONT.xs, false, COLORS.overlay0);
    }
    y += AXIS_H + 2.0 + SECTION_GAP;
    y += paint_cores(cr, x, y);
    y += SECTION_GAP;
    y += paint_rows(cr, x, y);
    y += SECTION_GAP;
    y += paint_apps(cr, x, y, w);
    return y + OUTER_MARGIN;
}

static double content_width(void) {
    double w = braille_graph_width(GRAPH_MIN_CELLS_W) + CARD_PAD * 2.0;
    double cores_w = core_cell_w() * 2.0 + CORE_COLUMN_GAP;
    if (cores_w > w) {
        w = cores_w;
    }

    char icon[5], model[96], counts[64];
    glyph_utf8(GLYPH_ICON_CPU, icon);
    short_model(model, sizeof(model));
    counts_text(counts, sizeof(counts));
    double header_w = text_width(icon, FONT.heading, false) + 8.0 + text_width(model, FONT.md, true) +
                      text_width(counts, FONT.sm, false) + 16.0 + text_width("active", FONT.xs, true) +
                      PILL_PAD_X * 2.0;
    if (header_w > w) {
        w = header_w;
    }

    char load[96], clock[64], uptime[48];
    row_texts(load, clock, uptime);
    // Task counts can grow while open; measure with some headroom.
    const char *values[] = {load, clock, "00.00 00.00 00.00 · 00 running / 00000 tasks"};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        double rw = label_col_w() + COL_GAP + text_width(values[i], FONT.label, false);
        if (rw > w) {
            w = rw;
        }
    }
    double apps_w = app_name_col_w() + APP_BAR_W + COL_GAP + text_width("100.0%", FONT.label, false);
    return apps_w > w ? apps_w : w;
}

static void draw_popup(cairo_t *cr, void *user_data) {
    (void)user_data;
    module_box_draw_rect(cr, 0, 0, g_popup.popup_width, g_popup.popup_height, COLORS.mantle, COLORS.peach,
                         (double)BAR.module_border_width);
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

void cpu_popup_open(Output *output, double box_x, double box_y, double box_w, double box_h) {
    (void)box_w; // left-anchored: only the box's left edge matters
    cpu_popup_close();
    gpu_popup_close();         // all three sysmon dropdowns open at the same spot
    mem_popup_close();

    // Start a fresh process measurement: this scan only primes it, the one
    // cpu_popup_tick() runs ~1s from now produces the first real figures.
    sysmon_cpu_reset_process_sample();
    sysmon_cpu_refresh_details();

    g_popup.output = output;
    g_popup.first_tick_done = false;
    g_popup.opened_at = monotonic_seconds();
    double content_w = content_width();
    g_popup.popup_width = content_w + OUTER_MARGIN * 2.0;
    g_popup.popup_height = paint_content(NULL, content_w);

    // TOP+LEFT, with the BAR.height correction every popup needs (see
    // CLAUDE.md "D-Bus / system tray"): the box lives in the bar's left
    // region, so the popup grows rightward from the box's left edge -- a
    // right anchor would push it off the left of the screen.
    int top_margin = (int)(box_y + box_h) - BAR.height;
    int left_margin = (int)box_x;
    g_popup.popup = popup_surface_create(output->app, output, (int)g_popup.popup_width, (int)g_popup.popup_height,
                                         top_margin, 0, 0, left_margin,
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
                                         ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
    if (!g_popup.popup) {
        return;
    }
    create_catchers(output->app);
    g_popup.open = true;
    popup_surface_render_now(g_popup.popup, draw_popup, NULL);
}

void cpu_popup_tick(void) {
    // Once only: the first clock tick at least MIN_FIRST_SAMPLE_S after
    // opening turns the priming scan into real process figures, instead of
    // "measuring..." for up to 5s until the next sysmon poll.
    if (!g_popup.open || g_popup.first_tick_done) {
        return;
    }
    if (monotonic_seconds() - g_popup.opened_at < MIN_FIRST_SAMPLE_S) {
        return;
    }
    sysmon_cpu_refresh_details();
    g_popup.first_tick_done = true;
    popup_surface_render_now(g_popup.popup, draw_popup, NULL);
}

void cpu_popup_refresh_if_open(void) {
    if (!g_popup.open) {
        return;
    }
    sysmon_cpu_refresh_details();
    popup_surface_render_now(g_popup.popup, draw_popup, NULL);
}

void cpu_popup_close(void) {
    if (!g_popup.open) {
        return;
    }
    popup_surface_destroy(g_popup.popup);
    g_popup.popup = NULL;
    destroy_catchers();
    g_popup.open = false;
    g_popup.output = NULL;
}

bool cpu_popup_is_menu_surface(struct wl_surface *surface) {
    return g_popup.open && g_popup.popup && popup_surface_wl_surface(g_popup.popup) == surface;
}

bool cpu_popup_is_catcher_surface(struct wl_surface *surface) {
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
