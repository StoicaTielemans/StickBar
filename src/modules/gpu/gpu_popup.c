#include "modules/gpu/gpu_popup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bar/popup_surface.h"
#include "config/glyphs.h"
#include "modules/cpu/cpu_popup.h"
#include "render/braille_graph.h"
#include "render/meter_bar.h"
#include "render/module_box.h"
#include "sysmon/sysmon_client.h"
#include "sysmon/sysmon_info_popup.h"

// Sized generously (this machine has 2 monitors), matching the same
// MAX_TRACKED_OUTPUTS convention used by every other popup in this project.
#define MAX_TRACKED_OUTPUTS 8

// Layout tuning, local to this popup (see CLAUDE.md "Conventions"). The
// section rhythm deliberately matches modules/network/network_info_popup.c.
#define OUTER_MARGIN 12.0
#define SECTION_GAP 10.0
#define HEADER_H 22.0
#define PILL_H 16.0
#define PILL_PAD_X 7.0
#define CARD_PAD 8.0
#define GRAPH_LABEL_H 14.0
#define GRAPH_LABEL_GAP 3.0
#define GRAPH_SPLIT_GAP 8.0 // between the usage and temperature graphs
#define GRAPH_CELLS_W (SYSMON_GPU_HISTORY_LEN / 2) // one sample per dot column
#define USAGE_CELLS_H 4
#define TEMP_CELLS_H 2
#define AXIS_H 12.0
#define ROW_H 17.0
#define COL_GAP 10.0
#define METER_W 110.0
#define METER_H 7.0
#define METER_SEGMENTS 10
#define APP_BAR_W 70.0
#define APP_BAR_H 5.0

// The temperature graph's range: idle cards sit around 30-45 °C and NVIDIA's
// slowdown point is in the 90s, so this span keeps both ends readable.
#define TEMP_GRAPH_MIN_C 30.0
#define TEMP_GRAPH_MAX_C 90.0

typedef struct {
    bool open;
    Output *output;
    PopupSurface *popup;
    double popup_width, popup_height;

    PopupSurface *catchers[MAX_TRACKED_OUTPUTS];
    struct wl_surface *catcher_surfaces[MAX_TRACKED_OUTPUTS];
    int catcher_count;
} GpuPopupState;

static GpuPopupState g_popup;

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

// "NVIDIA GeForce RTX 3080" -> "RTX 3080": the vendor/brand words add width
// without telling you anything on a machine that only has the one card.
static void short_gpu_name(char *out, size_t out_size) {
    const char *name = sysmon_gpu_name();
    static const char *prefixes[] = {"NVIDIA ", "GeForce "};
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t len = strlen(prefixes[i]);
        if (strncmp(name, prefixes[i], len) == 0) {
            name += len;
        }
    }
    snprintf(out, out_size, "%s", name[0] ? name : "NVIDIA GPU");
}

// P-state pill: P0 is full performance, P8 is idle (P12 deepest idle on
// some cards) -- three buckets are enough to read at a glance.
static const char *pstate_word(int p, Color *fill) {
    if (p <= 2) {
        *fill = COLORS.peach;
        return "busy";
    }
    if (p <= 7) {
        *fill = COLORS.yellow;
        return "active";
    }
    *fill = COLORS.green;
    return "idle";
}

// Shared by the meters and the usage graph: calm green filling toward hot
// red. A function rather than a static table, since COLORS isn't a
// compile-time constant (see CLAUDE.md "Power menu").
static void heat_gradient(Color out[3]) {
    out[0] = COLORS.green;
    out[1] = COLORS.yellow;
    out[2] = COLORS.red;
}

// ---- Rows (label + content) --------------------------------------------------

static const char *ROW_LABELS[] = {"VRAM", "POWER", "FAN", "CLOCK", "VIDEO"};

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

// Name column of the apps list: /proc/PID/comm names are at most 15
// characters (the kernel's TASK_COMM_LEN minus the NUL), and the font is
// monospace, so 15 of any glyph is exactly the widest a name can get.
static double app_name_col_w(void) {
    return text_width("000000000000000", FONT.label, false) + COL_GAP;
}

static void row_label(cairo_t *cr, double x, double y, const char *label) {
    module_box_draw_text_left(cr, x, y, ROW_H, label, FONT.xs, true, COLORS.overlay0);
}

// The three meter rows' value texts, built in one place so measuring and
// drawing agree.
static void meter_texts(char vram[48], char power[48], char fan[48]) {
    snprintf(vram, 48, "%.1f / %.1f GB", sysmon_gpu_mem_used_mb() / 1024.0, sysmon_gpu_mem_total_mb() / 1024.0);
    if (sysmon_gpu_power_limit_w() > 0.0) {
        snprintf(power, 48, "%.0f / %.0f W", sysmon_gpu_power_w(), sysmon_gpu_power_limit_w());
    } else {
        snprintf(power, 48, "%.0f W", sysmon_gpu_power_w());
    }
    if (sysmon_gpu_fan_percent() == 0) {
        snprintf(fan, 48, "0%% · stopped");
    } else {
        snprintf(fan, 48, "%d%%", sysmon_gpu_fan_percent());
    }
}

static void line_texts(char clock[64], char video[64]) {
    if (sysmon_gpu_clock_max_mhz() > 0) {
        snprintf(clock, 64, "%d / %d MHz · mem %d MHz", sysmon_gpu_clock_mhz(), sysmon_gpu_clock_max_mhz(),
                 sysmon_gpu_mem_clock_mhz());
    } else {
        snprintf(clock, 64, "%d MHz · mem %d MHz", sysmon_gpu_clock_mhz(), sysmon_gpu_mem_clock_mhz());
    }
    snprintf(video, 64, "enc %d%% · dec %d%%", sysmon_gpu_encoder_percent(), sysmon_gpu_decoder_percent());
}

// ---- Painting ----------------------------------------------------------------
// Every paint_* takes a NULL `cr` to mean "measure only" and returns the
// height it uses, so the size computed at open and the drawn layout can't
// drift apart (same approach as network_info_popup.c).

static double paint_header(cairo_t *cr, double x, double y, double w) {
    if (!cr) {
        return HEADER_H;
    }
    char icon[5];
    glyph_utf8(GLYPH_ICON_GPU, icon);
    module_box_draw_text_left(cr, x, y, HEADER_H, icon, FONT.heading, false, COLORS.green);
    double tx = x + text_width(icon, FONT.heading, false) + 8.0;

    if (!sysmon_gpu_available()) {
        module_box_draw_text_left(cr, tx, y, HEADER_H, "No NVIDIA GPU detected", FONT.md, true, COLORS.text);
        return HEADER_H;
    }

    char name[96];
    short_gpu_name(name, sizeof(name));
    module_box_draw_text_left(cr, tx, y, HEADER_H, name, FONT.md, true, COLORS.text);
    tx += text_width(name, FONT.md, true);
    if (sysmon_gpu_driver()[0]) {
        char driver[96];
        snprintf(driver, sizeof(driver), "  ·  driver %s", sysmon_gpu_driver());
        module_box_draw_text_left(cr, tx, y, HEADER_H, driver, FONT.sm, false, COLORS.subtext0);
    }

    const char *pstate = sysmon_gpu_pstate();
    if (pstate[0] == 'P') {
        Color fill;
        const char *word = pstate_word(atoi(pstate + 1), &fill);
        char pill[32];
        snprintf(pill, sizeof(pill), "%s %s", pstate, word);
        double pill_w = text_width(pill, FONT.xs, true) + PILL_PAD_X * 2.0;
        double pill_x = x + w - pill_w;
        double pill_y = y + (HEADER_H - PILL_H) / 2.0;
        module_box_draw_rect(cr, pill_x, pill_y, pill_w, PILL_H, fill, COLORS.mantle, 0.0);
        module_box_draw_text_centered(cr, pill_x, pill_y, pill_w, PILL_H, pill, FONT.xs, true, COLORS.mantle);
    }
    return HEADER_H;
}

static void graph_label_row(cairo_t *cr, double x, double y, double w, const char *left, Color left_color,
                            const char *right) {
    module_box_draw_text_left(cr, x, y, GRAPH_LABEL_H, left, FONT.sm, true, left_color);
    draw_text_right(cr, x + w, y, GRAPH_LABEL_H, right, FONT.xs, false, COLORS.overlay0);
}

// Graph card: usage (rising, 0-100%) over temperature (rising, 30-90 °C),
// each with a "now / peak" label row above it.
static double paint_graph_card(cairo_t *cr, double x, double y, double w) {
    double graph_w = braille_graph_width(GRAPH_CELLS_W);
    double usage_h = braille_graph_height(USAGE_CELLS_H);
    double temp_h = braille_graph_height(TEMP_CELLS_H);
    double h = CARD_PAD * 2.0 + (GRAPH_LABEL_H + GRAPH_LABEL_GAP) * 2.0 + usage_h + GRAPH_SPLIT_GAP + temp_h;
    if (!cr) {
        return h;
    }
    module_box_draw_rect(cr, x, y, w, h, COLORS.base, COLORS.surface1, 1.0);

    int n = sysmon_gpu_history_len();
    double usage[SYSMON_GPU_HISTORY_LEN], temp[SYSMON_GPU_HISTORY_LEN];
    double usage_peak = 0.0, temp_peak = 0.0;
    for (int i = 0; i < n; i++) {
        usage[i] = sysmon_gpu_util_history(i);
        double t = sysmon_gpu_temp_history(i);
        // The graph's zero is TEMP_GRAPH_MIN_C, not 0 °C -- otherwise an idle
        // card's 40 °C would already fill two thirds of it.
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

    snprintf(left, sizeof(left), "usage %d%%", sysmon_gpu_percent());
    snprintf(right, sizeof(right), "peak %.0f%%", usage_peak);
    graph_label_row(cr, inner_x, cy, inner_w, left, COLORS.green, right);
    cy += GRAPH_LABEL_H + GRAPH_LABEL_GAP;
    Color usage_gradient[3];
    heat_gradient(usage_gradient);
    braille_graph_draw(cr, graph_x, cy, GRAPH_CELLS_W, USAGE_CELLS_H, usage, n, 100.0, true, usage_gradient);
    cy += usage_h + GRAPH_SPLIT_GAP;

    snprintf(left, sizeof(left), "temp %d°C", sysmon_gpu_temp_c());
    snprintf(right, sizeof(right), "max %.0f°C", temp_peak);
    graph_label_row(cr, inner_x, cy, inner_w, left, COLORS.peach, right);
    cy += GRAPH_LABEL_H + GRAPH_LABEL_GAP;
    const Color temp_gradient[3] = {COLORS.peach, COLORS.maroon, COLORS.red};
    braille_graph_draw(cr, graph_x, cy, GRAPH_CELLS_W, TEMP_CELLS_H, temp, n, TEMP_GRAPH_MAX_C - TEMP_GRAPH_MIN_C,
                       true, temp_gradient);
    return h;
}

static void meter_row(cairo_t *cr, double x, double y, double value_x, const char *label, double fraction,
                      const char *value) {
    row_label(cr, x, y, label);
    Color gradient[3];
    heat_gradient(gradient);
    meter_bar_draw(cr, value_x, y + (ROW_H - METER_H) / 2.0, METER_W, METER_H, fraction, METER_SEGMENTS, gradient,
                   COLORS.surface0);
    module_box_draw_text_left(cr, value_x + METER_W + COL_GAP, y, ROW_H, value, FONT.label, false, COLORS.text);
}

static double paint_rows(cairo_t *cr, double x, double y) {
    if (!cr) {
        return ROW_H * 5.0;
    }
    double value_x = x + label_col_w() + COL_GAP;
    char vram[48], power[48], fan[48], clock[64], video[64];
    meter_texts(vram, power, fan);
    line_texts(clock, video);

    int total = sysmon_gpu_mem_total_mb();
    double limit = sysmon_gpu_power_limit_w();
    meter_row(cr, x, y, value_x, "VRAM", total > 0 ? (double)sysmon_gpu_mem_used_mb() / total : 0.0, vram);
    meter_row(cr, x, y + ROW_H, value_x, "POWER", limit > 0.0 ? sysmon_gpu_power_w() / limit : 0.0, power);
    meter_row(cr, x, y + ROW_H * 2.0, value_x, "FAN", sysmon_gpu_fan_percent() / 100.0, fan);

    row_label(cr, x, y + ROW_H * 3.0, "CLOCK");
    module_box_draw_text_left(cr, value_x, y + ROW_H * 3.0, ROW_H, clock, FONT.label, false, COLORS.text);
    row_label(cr, x, y + ROW_H * 4.0, "VIDEO");
    module_box_draw_text_left(cr, value_x, y + ROW_H * 4.0, ROW_H, video, FONT.label, false, COLORS.text);
    return ROW_H * 5.0;
}

// "TOP VRAM APPS": always SYSMON_GPU_MAX_PROCS rows tall so the size fixed at
// open never clips when more apps start using the GPU while it's open.
static double paint_apps(cairo_t *cr, double x, double y, double w) {
    double h = ROW_H * (1 + SYSMON_GPU_MAX_PROCS);
    if (!cr) {
        return h;
    }
    module_box_draw_text_left(cr, x, y, ROW_H, "TOP VRAM APPS", FONT.xs, true, COLORS.overlay0);
    int count = sysmon_gpu_process_count();
    if (count == 0) {
        module_box_draw_text_left(cr, x, y + ROW_H, ROW_H, "no apps using VRAM", FONT.label, false,
                                  COLORS.subtext0);
        return h;
    }
    // Bars are relative to the biggest app (the first), not to total VRAM:
    // a 400 MiB app on a 10 GB card would otherwise be a sliver.
    int biggest = sysmon_gpu_process_mib(0);
    const Color app_gradient[3] = {COLORS.green, COLORS.green, COLORS.green};
    for (int i = 0; i < count; i++) {
        double ry = y + ROW_H * (i + 1);
        module_box_draw_text_left(cr, x, ry, ROW_H, sysmon_gpu_process_name(i), FONT.label, false, COLORS.text);
        double fraction = biggest > 0 ? (double)sysmon_gpu_process_mib(i) / biggest : 0.0;
        meter_bar_draw(cr, x + app_name_col_w(), ry + (ROW_H - APP_BAR_H) / 2.0, APP_BAR_W, APP_BAR_H, fraction, 7,
                       app_gradient, COLORS.surface0);
        char mib[32];
        snprintf(mib, sizeof(mib), "%d MiB", sysmon_gpu_process_mib(i));
        draw_text_right(cr, x + w, ry, ROW_H, mib, FONT.label, false, COLORS.subtext0);
    }
    return h;
}

static double paint_content(cairo_t *cr, double w) {
    double x = OUTER_MARGIN, y = OUTER_MARGIN;
    y += paint_header(cr, x, y, w);
    if (!sysmon_gpu_available()) {
        if (cr) {
            module_box_draw_text_left(cr, x, y, ROW_H, "NVML unavailable -- no NVIDIA driver?", FONT.sm, false,
                                      COLORS.subtext0);
        }
        return y + ROW_H + OUTER_MARGIN;
    }
    y += SECTION_GAP;
    y += paint_graph_card(cr, x, y, w);
    if (cr) {
        module_box_draw_text_left(cr, x, y + 2.0, AXIS_H, "5 min ago", FONT.xs, false, COLORS.overlay0);
        draw_text_right(cr, x + w, y + 2.0, AXIS_H, "now", FONT.xs, false, COLORS.overlay0);
    }
    y += AXIS_H + 2.0 + SECTION_GAP;
    y += paint_rows(cr, x, y);
    y += SECTION_GAP;
    y += paint_apps(cr, x, y, w);
    return y + OUTER_MARGIN;
}

// The graph card's natural width, widened if the header or a row needs more.
static double content_width(void) {
    double w = braille_graph_width(GRAPH_CELLS_W) + CARD_PAD * 2.0;
    char icon[5];
    glyph_utf8(GLYPH_ICON_GPU, icon);
    double icon_w = text_width(icon, FONT.heading, false) + 8.0;
    if (!sysmon_gpu_available()) {
        double hw = icon_w + text_width("No NVIDIA GPU detected", FONT.md, true);
        return hw > w ? hw : w;
    }

    char name[96], driver[96];
    short_gpu_name(name, sizeof(name));
    snprintf(driver, sizeof(driver), "  ·  driver %s", sysmon_gpu_driver());
    double header_w = icon_w + text_width(name, FONT.md, true) + text_width(driver, FONT.sm, false) + 16.0 +
                      text_width("P12 active", FONT.xs, true) + PILL_PAD_X * 2.0;
    if (header_w > w) {
        w = header_w;
    }

    double value_x = label_col_w() + COL_GAP;
    char vram[48], power[48], fan[48], clock[64], video[64];
    meter_texts(vram, power, fan);
    line_texts(clock, video);
    // Meter values can grow while open (e.g. power going 36 -> 300 W), so
    // measure against a wide sample rather than the current reading.
    const char *meter_values[] = {vram, power, fan, "000 / 000 W", "100%"};
    for (size_t i = 0; i < sizeof(meter_values) / sizeof(meter_values[0]); i++) {
        double rw = value_x + METER_W + COL_GAP + text_width(meter_values[i], FONT.label, false);
        if (rw > w) {
            w = rw;
        }
    }
    const char *lines[] = {clock, video, "0000 / 0000 MHz · mem 00000 MHz"};
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        double rw = value_x + text_width(lines[i], FONT.label, false);
        if (rw > w) {
            w = rw;
        }
    }
    double apps_w = app_name_col_w() + APP_BAR_W + COL_GAP + text_width("00000 MiB", FONT.label, false);
    return apps_w > w ? apps_w : w;
}

static void draw_popup(cairo_t *cr, void *user_data) {
    (void)user_data;
    module_box_draw_rect(cr, 0, 0, g_popup.popup_width, g_popup.popup_height, COLORS.mantle, COLORS.green,
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

void gpu_popup_open(Output *output, double box_x, double box_y, double box_w, double box_h) {
    (void)box_w; // left-anchored: only the box's left edge matters
    gpu_popup_close();
    sysmon_info_popup_close(); // same drop-down spot as the CPU/memory popups
    cpu_popup_close();

    // VRAM/fan/power/clocks/apps aren't polled in the background (see
    // sysmon_gpu_refresh_details()) -- fetch them now, before measuring.
    sysmon_gpu_refresh_details();

    g_popup.output = output;
    double content_w = content_width();
    g_popup.popup_width = content_w + OUTER_MARGIN * 2.0;
    g_popup.popup_height = paint_content(NULL, content_w);

    // Same TOP+LEFT anchor math (including the BAR.height correction) as
    // sysmon/sysmon_info_popup.c -- see its comment for why left-anchored.
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

void gpu_popup_refresh_if_open(void) {
    if (!g_popup.open) {
        return;
    }
    // ~4-11ms (the fan query dominates), but only every 5s and only while
    // the dropdown is actually open.
    sysmon_gpu_refresh_details();
    popup_surface_render_now(g_popup.popup, draw_popup, NULL);
}

void gpu_popup_close(void) {
    if (!g_popup.open) {
        return;
    }
    popup_surface_destroy(g_popup.popup);
    g_popup.popup = NULL;
    destroy_catchers();
    g_popup.open = false;
    g_popup.output = NULL;
}

bool gpu_popup_is_menu_surface(struct wl_surface *surface) {
    return g_popup.open && g_popup.popup && popup_surface_wl_surface(g_popup.popup) == surface;
}

bool gpu_popup_is_catcher_surface(struct wl_surface *surface) {
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
