#include "modules/gpu/gpu_popup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bar/popup_host.h"
#include "bar/popup_surface.h"
#include "config/glyphs.h"
#include "modules/cpu/cpu_popup.h"
#include "modules/mem/mem_popup.h"
#include "render/braille_graph.h"
#include "render/meter_bar.h"
#include "render/module_box.h"
#include "render/popup_chrome.h"
#include "render/popup_style.h"
#include "render/stat_card.h"
#include "sysmon/sysmon_client.h"

// Layout tuning, local to this popup (see CLAUDE.md "Conventions"). The
// section rhythm deliberately matches modules/network/network_info_popup.c.
#define USAGE_CELLS_H 4
#define TEMP_CELLS_H 2
#define METER_W 110.0
#define METER_H 7.0
#define METER_SEGMENTS 10

// The temperature graph's range: idle cards sit around 30-45 °C and NVIDIA's
// slowdown point is in the 90s, so this span keeps both ends readable.
#define TEMP_GRAPH_MIN_C 30.0
#define TEMP_GRAPH_MAX_C 90.0

typedef struct {
    PopupHost host; // the dropdown surface, its placement and click-catchers
    double popup_width, popup_height;

} GpuPopupState;

static GpuPopupState g_popup;

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
// compile-time constant (see docs/modules/power-menu.md).
static void heat_gradient(Color out[3]) {
    out[0] = COLORS.green;
    out[1] = COLORS.yellow;
    out[2] = COLORS.red;
}

// ---- Rows (label + content) --------------------------------------------------

static const char *ROW_LABELS[] = {"VRAM", "POWER", "FAN", "CLOCK", "VIDEO"};

static double label_col_w(void) {
    return stat_label_col_w(ROW_LABELS, sizeof(ROW_LABELS) / sizeof(ROW_LABELS[0]));
}

static void row_label(cairo_t *cr, double x, double y, const char *label) {
    module_box_draw_text_left(cr, x, y, STAT_ROW_H, label, FONT.xs, true, COLORS.overlay0);
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
        return POPUP_HEADER_H;
    }
    char icon[5];
    glyph_utf8(GLYPH_ICON_GPU, icon);
    PopupHeader header = {.icon = icon, .icon_color = COLORS.green, .title = "No NVIDIA GPU detected"};
    char name[96], driver[96], pill[32];
    if (sysmon_gpu_available()) {
        short_gpu_name(name, sizeof(name));
        header.title = name;
        if (sysmon_gpu_driver()[0]) {
            snprintf(driver, sizeof(driver), "  ·  driver %s", sysmon_gpu_driver());
            header.summary = driver;
        }
        const char *pstate = sysmon_gpu_pstate();
        if (pstate[0] == 'P') {
            const char *word = pstate_word(atoi(pstate + 1), &header.pill_fill);
            snprintf(pill, sizeof(pill), "%s %s", pstate, word);
            header.pill = pill;
        }
    }
    popup_draw_header(cr, x, y, w, &header);
    return POPUP_HEADER_H;
}

// Graph card: usage (rising, 0-100%) over temperature (rising, 30-90 °C),
// each with a "now / peak" label row above it.
static double paint_graph_card(cairo_t *cr, double x, double y, double w) {
    int cells = stat_graph_cells(w, SYSMON_GPU_HISTORY_LEN);
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
    int first = stat_history_window(sysmon_gpu_history_len(), cells, &n);
    double usage[SYSMON_GPU_HISTORY_LEN], temp[SYSMON_GPU_HISTORY_LEN];
    double usage_peak = 0.0, temp_peak = 0.0;
    for (int i = 0; i < n; i++) {
        usage[i] = sysmon_gpu_util_history(first + i);
        double t = sysmon_gpu_temp_history(first + i);
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

    double inner_x = x + STAT_CARD_PAD, inner_w = w - STAT_CARD_PAD * 2.0;
    double graph_x = x + (w - graph_w) / 2.0;
    double cy = y + STAT_CARD_PAD;
    char left[48], right[48];

    snprintf(left, sizeof(left), "usage %d%%", sysmon_gpu_percent());
    snprintf(right, sizeof(right), "peak %.0f%%", usage_peak);
    stat_graph_label_row(cr, inner_x, cy, inner_w, left, COLORS.green, right);
    cy += STAT_GRAPH_LABEL_H + STAT_GRAPH_LABEL_GAP;
    Color usage_gradient[3];
    heat_gradient(usage_gradient);
    braille_graph_draw(cr, graph_x, cy, cells, USAGE_CELLS_H, usage, n, 100.0, true, usage_gradient);
    cy += usage_h + STAT_GRAPH_SPLIT_GAP;

    snprintf(left, sizeof(left), "temp %d°C", sysmon_gpu_temp_c());
    snprintf(right, sizeof(right), "max %.0f°C", temp_peak);
    stat_graph_label_row(cr, inner_x, cy, inner_w, left, COLORS.peach, right);
    cy += STAT_GRAPH_LABEL_H + STAT_GRAPH_LABEL_GAP;
    const Color temp_gradient[3] = {COLORS.peach, COLORS.maroon, COLORS.red};
    braille_graph_draw(cr, graph_x, cy, cells, TEMP_CELLS_H, temp, n, TEMP_GRAPH_MAX_C - TEMP_GRAPH_MIN_C,
                       true, temp_gradient);
    return h;
}

static void meter_row(cairo_t *cr, double x, double y, double value_x, const char *label, double fraction,
                      const char *value) {
    row_label(cr, x, y, label);
    Color gradient[3];
    heat_gradient(gradient);
    meter_bar_draw(cr, value_x, y + (STAT_ROW_H - METER_H) / 2.0, METER_W, METER_H, fraction, METER_SEGMENTS, gradient,
                   COLORS.surface0);
    module_box_draw_text_left(cr, value_x + METER_W + POPUP_COL_GAP, y, STAT_ROW_H, value, FONT.label, false,
                              COLORS.text);
}

static double paint_rows(cairo_t *cr, double x, double y) {
    if (!cr) {
        return STAT_ROW_H * 5.0;
    }
    double value_x = x + label_col_w() + POPUP_COL_GAP;
    char vram[48], power[48], fan[48], clock[64], video[64];
    meter_texts(vram, power, fan);
    line_texts(clock, video);

    int total = sysmon_gpu_mem_total_mb();
    double limit = sysmon_gpu_power_limit_w();
    meter_row(cr, x, y, value_x, "VRAM", total > 0 ? (double)sysmon_gpu_mem_used_mb() / total : 0.0, vram);
    meter_row(cr, x, y + STAT_ROW_H, value_x, "POWER", limit > 0.0 ? sysmon_gpu_power_w() / limit : 0.0, power);
    meter_row(cr, x, y + STAT_ROW_H * 2.0, value_x, "FAN", sysmon_gpu_fan_percent() / 100.0, fan);

    row_label(cr, x, y + STAT_ROW_H * 3.0, "CLOCK");
    module_box_draw_text_left(cr, value_x, y + STAT_ROW_H * 3.0, STAT_ROW_H, clock, FONT.label, false, COLORS.text);
    row_label(cr, x, y + STAT_ROW_H * 4.0, "VIDEO");
    module_box_draw_text_left(cr, value_x, y + STAT_ROW_H * 4.0, STAT_ROW_H, video, FONT.label, false, COLORS.text);
    return STAT_ROW_H * 5.0;
}

// "TOP VRAM APPS": always SYSMON_GPU_MAX_PROCS rows tall so the size fixed at
// open never clips when more apps start using the GPU while it's open.
static double paint_apps(cairo_t *cr, double x, double y, double w) {
    double h = STAT_ROW_H * (1 + SYSMON_GPU_MAX_PROCS);
    if (!cr) {
        return h;
    }
    module_box_draw_text_left(cr, x, y, STAT_ROW_H, "TOP VRAM APPS", FONT.xs, true, COLORS.overlay0);
    int count = sysmon_gpu_process_count();
    if (count == 0) {
        module_box_draw_text_left(cr, x, y + STAT_ROW_H, STAT_ROW_H, "no apps using VRAM", FONT.label, false,
                                  COLORS.subtext0);
        return h;
    }
    // Bars are relative to the biggest app (the first), not to total VRAM:
    // a 400 MiB app on a 10 GB card would otherwise be a sliver.
    int biggest = sysmon_gpu_process_mib(0);
    for (int i = 0; i < count; i++) {
        double ry = y + STAT_ROW_H * (i + 1);
        double fraction = biggest > 0 ? (double)sysmon_gpu_process_mib(i) / biggest : 0.0;
        char mib[32];
        snprintf(mib, sizeof(mib), "%d MiB", sysmon_gpu_process_mib(i));
        stat_draw_app_row(cr, x, ry, w, sysmon_gpu_process_name(i), fraction, COLORS.green, mib);
    }
    return h;
}

static double paint_content(cairo_t *cr, double w) {
    double x = POPUP_OUTER_MARGIN, y = POPUP_OUTER_MARGIN;
    y += paint_header(cr, x, y, w);
    if (!sysmon_gpu_available()) {
        if (cr) {
            module_box_draw_text_left(cr, x, y, STAT_ROW_H, "NVML unavailable -- no NVIDIA driver?", FONT.sm, false,
                                      COLORS.subtext0);
        }
        return y + STAT_ROW_H + POPUP_OUTER_MARGIN;
    }
    y += POPUP_SECTION_GAP;
    y += paint_graph_card(cr, x, y, w);
    y += stat_graph_axis(cr, x, y, w, stat_graph_cells(w, SYSMON_GPU_HISTORY_LEN)) + POPUP_SECTION_GAP;
    y += paint_rows(cr, x, y);
    y += POPUP_SECTION_GAP;
    y += paint_apps(cr, x, y, w);
    return y + POPUP_OUTER_MARGIN;
}

// The graph card's natural width, widened if the header or a row needs more.
static double content_width(void) {
    double w = braille_graph_width(STAT_GRAPH_MIN_CELLS_W) + STAT_CARD_PAD * 2.0;
    char icon[5];
    glyph_utf8(GLYPH_ICON_GPU, icon);
    if (!sysmon_gpu_available()) {
        PopupHeader none = {.icon = icon, .title = "No NVIDIA GPU detected"};
        double hw = popup_header_width(&none);
        return hw > w ? hw : w;
    }

    char name[96], driver[96];
    short_gpu_name(name, sizeof(name));
    snprintf(driver, sizeof(driver), "  ·  driver %s", sysmon_gpu_driver());
    // Sized for the widest pill text.
    PopupHeader header = {.icon = icon, .title = name, .summary = driver, .pill = "P12 active"};
    double header_w = popup_header_width(&header);
    if (header_w > w) {
        w = header_w;
    }

    double value_x = label_col_w() + POPUP_COL_GAP;
    char vram[48], power[48], fan[48], clock[64], video[64];
    meter_texts(vram, power, fan);
    line_texts(clock, video);
    // Meter values can grow while open (e.g. power going 36 -> 300 W), so
    // measure against a wide sample rather than the current reading.
    const char *meter_values[] = {vram, power, fan, "000 / 000 W", "100%"};
    for (size_t i = 0; i < sizeof(meter_values) / sizeof(meter_values[0]); i++) {
        double rw = value_x + METER_W + POPUP_COL_GAP + module_box_text_width(meter_values[i], FONT.label, false);
        if (rw > w) {
            w = rw;
        }
    }
    const char *lines[] = {clock, video, "0000 / 0000 MHz · mem 00000 MHz"};
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        double rw = value_x + module_box_text_width(lines[i], FONT.label, false);
        if (rw > w) {
            w = rw;
        }
    }
    double apps_w = stat_app_name_col_w() + STAT_APP_BAR_W + POPUP_COL_GAP + module_box_text_width("00000 MiB",
                                                                                                   FONT.label, false);
    return apps_w > w ? apps_w : w;
}

static void draw_popup(cairo_t *cr, void *user_data) {
    (void)user_data;
    popup_draw_frame(cr, g_popup.popup_width, g_popup.popup_height, COLORS.green);
    paint_content(cr, g_popup.popup_width - POPUP_OUTER_MARGIN * 2.0);
}

void gpu_popup_open(Output *output, double box_x, double box_y, double box_w, double box_h) {
    gpu_popup_close();
    cpu_popup_close(); // all three sysmon dropdowns open at the same spot
    mem_popup_close();

    // VRAM/fan/power/clocks/apps aren't polled in the background (see
    // sysmon_gpu_refresh_details()) -- fetch them now, before measuring.
    sysmon_gpu_refresh_details();

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

void gpu_popup_refresh_if_open(void) {
    if (!g_popup.host.open) {
        return;
    }
    // ~4-11ms (the fan query dominates), but only every 5s and only while
    // the dropdown is actually open.
    sysmon_gpu_refresh_details();
    popup_surface_render_now(g_popup.host.popup, draw_popup, NULL);
}

void gpu_popup_close(void) {
    if (!g_popup.host.open) {
        return;
    }
    popup_host_close(&g_popup.host);
}

bool gpu_popup_is_menu_surface(struct wl_surface *surface) {
    return popup_host_is_menu_surface(&g_popup.host, surface);
}

bool gpu_popup_is_catcher_surface(struct wl_surface *surface) {
    return popup_host_is_catcher_surface(&g_popup.host, surface);
}
