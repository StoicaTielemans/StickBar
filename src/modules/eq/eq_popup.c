#include "modules/eq/eq_popup.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bar/popup_surface.h"
#include "config/glyphs.h"
#include "eq/eq_curve.h"
#include "eq/eq_state.h"
#include "render/module_box.h"
#include "wayland/output.h"

// Sized generously (this machine has 2 monitors), matching the same
// MAX_TRACKED_OUTPUTS convention used by every other popup in this project.
#define MAX_TRACKED_OUTPUTS 8

// The profiles offered in the dropdown, in display order -- a deliberate
// subset of DacController's presets/ (the user asked for exactly these
// three: the IEM profile and the two DT 990 variants worth switching
// between day to day). Each name must match a presets/NAME.txt file, since
// it's passed straight to `blackpearl-eq.py profile NAME`.
typedef struct {
    const char *name;
    const char *description;
} EqProfileEntry;

static const EqProfileEntry PROFILES[] = {
    { "zero2",      "Zero:2 IEM - low gain" },
    { "dt990",      "DT 990 - full EQ, more bass" },
    { "dt990-loud", "DT 990 - treble fix, louder" },
};
#define PROFILE_COUNT ((int)(sizeof(PROFILES) / sizeof(PROFILES[0])))

// Geometry constants local to this widget (see CLAUDE.md "Conventions").
#define OUTER_MARGIN 8.0       // matches Config.bar.popupPadding, same as every other popup
#define SECTION_GAP 8.0        // between header, graph, and the profile rows
#define ROW_SPACING 6.0
#define ROW_INNER_SPACING 10.0
#define GRAPH_W 300.0
#define GRAPH_H 120.0
#define DB_LABEL_W 28.0        // left column for the "+6"/"-6" grid labels
#define FREQ_LABEL_GAP 2.0     // between the graph's bottom edge and the Hz labels
// The graph's dB span: at least +/-12 dB, growing in 6 dB grid steps to fit
// heavier EQs (e.g. the bass-v preset's +19 dB) without clipping them.
#define GRAPH_MIN_RANGE_DB 12.0
#define GRAPH_STEP_DB 6.0

typedef struct {
    double y, h;
} RowLayout;

typedef struct {
    bool open;
    Output *output;
    PopupSurface *popup;

    char header[160];
    double header_y, header_h;
    double graph_x, graph_y;
    double freq_label_h;
    RowLayout rows[PROFILE_COUNT];
    double marker_col_w, content_width, popup_width, popup_height;
    int hovered_index;

    PopupSurface *catchers[MAX_TRACKED_OUTPUTS];
    struct wl_surface *catcher_surfaces[MAX_TRACKED_OUTPUTS];
    int catcher_count;
} EqPopupState;

static EqPopupState g_popup = { .hovered_index = -1 };

static void build_header(char *out, size_t out_size) {
    if (!eq_state_available()) {
        snprintf(out, out_size, "No EQ state yet - run blackpearl-eq.py state");
        return;
    }
    const char *profile = eq_state_profile()[0] ? eq_state_profile() : "custom EQ";
    const char *gain = eq_state_gain()[0] ? eq_state_gain() : "?";
    snprintf(out, out_size, "%s  |  %s gain  |  preamp %+.0f dB%s", profile, gain,
             eq_state_preamp(), eq_state_enabled() ? "" : "  |  EQ OFF");
}

static void compute_layout(void) {
    build_header(g_popup.header, sizeof(g_popup.header));

    double w, h;
    module_box_measure_text(g_popup.header, FONT.md, true, &w, &h);
    g_popup.header_y = OUTER_MARGIN;
    g_popup.header_h = h;

    g_popup.graph_x = OUTER_MARGIN + DB_LABEL_W;
    g_popup.graph_y = g_popup.header_y + g_popup.header_h + SECTION_GAP;
    module_box_measure_text("10k", FONT.xs, false, &w, &g_popup.freq_label_h);

    char buf[5];
    double mw1, mh1, mw2, mh2;
    glyph_utf8(GLYPH_ICON_DEVICE_DEFAULT, buf);
    module_box_measure_text(buf, FONT.heading, false, &mw1, &mh1);
    glyph_utf8(GLYPH_ICON_DEVICE_SELECTABLE, buf);
    module_box_measure_text(buf, FONT.heading, false, &mw2, &mh2);
    g_popup.marker_col_w = mw1 > mw2 ? mw1 : mw2;
    double row_h = mh1 > mh2 ? mh1 : mh2;
    module_box_measure_text("Ag", FONT.label, false, &w, &h);
    if (h > row_h) row_h = h;

    // Content width is the graph's (plus its dB label column) -- the header
    // and rows are short enough to fit within it at these font sizes.
    g_popup.content_width = DB_LABEL_W + GRAPH_W;
    module_box_measure_text(g_popup.header, FONT.md, true, &w, &h);
    if (w > g_popup.content_width) {
        g_popup.content_width = w;
    }

    double y = g_popup.graph_y + GRAPH_H + FREQ_LABEL_GAP + g_popup.freq_label_h + SECTION_GAP;
    for (int i = 0; i < PROFILE_COUNT; i++) {
        g_popup.rows[i].y = y;
        g_popup.rows[i].h = row_h;
        y += row_h + (i < PROFILE_COUNT - 1 ? ROW_SPACING : 0.0);
    }
    g_popup.popup_width = g_popup.content_width + OUTER_MARGIN * 2.0;
    g_popup.popup_height = y + OUTER_MARGIN;
}

static void draw_transparent(cairo_t *cr, void *user_data) {
    (void)user_data;
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

static void stroke_line(cairo_t *cr, double x0, double y0, double x1, double y1, Color c) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x0, y0);
    cairo_line_to(cr, x1, y1);
    cairo_stroke(cr);
}

// Background, dB grid (every GRAPH_STEP_DB, 0 dB brighter) with labels on the
// left, decade frequency grid (100 Hz / 1 kHz / 10 kHz) with labels below,
// then the curve itself on top.
static void draw_graph(cairo_t *cr) {
    double gx = g_popup.graph_x, gy = g_popup.graph_y;
    double range = eq_curve_range_db(GRAPH_MIN_RANGE_DB, GRAPH_STEP_DB);
    double mid_y = gy + GRAPH_H / 2.0;
    double px_per_db = (GRAPH_H / 2.0) / range;

    module_box_draw_rect(cr, gx, gy, GRAPH_W, GRAPH_H, COLORS.base, (Color){ 0, 0, 0, 0 }, 0.0);

    for (double db = -range; db <= range + 0.001; db += GRAPH_STEP_DB) {
        // +0.5: land 1px lines on a pixel row (see eq_curve.c's zero line).
        double y = floor(mid_y - db * px_per_db) + 0.5;
        if (fabs(db) > 0.001) {
            stroke_line(cr, gx, y, gx + GRAPH_W, y, COLORS.surface0);
        }
        char label[8];
        snprintf(label, sizeof(label), "%+.0f", db);
        if (fabs(db) < 0.001) {
            snprintf(label, sizeof(label), "0");
        }
        double lw, lh;
        module_box_measure_text(label, FONT.xs, false, &lw, &lh);
        module_box_draw_text_left(cr, gx - 4.0 - lw, y - lh / 2.0, lh, label, FONT.xs, false, COLORS.overlay1);
    }

    static const double DECADES[] = { 100.0, 1000.0, 10000.0 };
    static const char *DECADE_LABELS[] = { "100", "1k", "10k" };
    for (int i = 0; i < 3; i++) {
        double x = floor(eq_curve_freq_to_x(DECADES[i], gx, GRAPH_W)) + 0.5;
        stroke_line(cr, x, gy, x, gy + GRAPH_H, COLORS.surface0);
        double lw, lh;
        module_box_measure_text(DECADE_LABELS[i], FONT.xs, false, &lw, &lh);
        module_box_draw_text_left(cr, x - lw / 2.0, gy + GRAPH_H + FREQ_LABEL_GAP, lh,
                                   DECADE_LABELS[i], FONT.xs, false, COLORS.overlay1);
    }

    Color accent = eq_state_enabled() ? COLORS.lavender : COLORS.overlay0;
    eq_curve_draw(cr, gx, gy, GRAPH_W, GRAPH_H, range, accent, 2.0, true, COLORS.surface2);
}

static void draw_row(cairo_t *cr, int i) {
    RowLayout *row = &g_popup.rows[i];
    const char *name = PROFILES[i].name;
    bool active = strcmp(eq_state_profile(), name) == 0;
    bool pending = strcmp(eq_state_pending_profile(), name) == 0;

    if (i == g_popup.hovered_index) {
        module_box_draw_rect(cr, OUTER_MARGIN, row->y, g_popup.content_width, row->h,
                              COLORS.surface1, (Color){ 0, 0, 0, 0 }, 0.0);
    }

    // Same filled/hollow marker glyph pair audio/device_popup.c uses for
    // "current default device" vs. "selectable", for the same meaning here.
    char glyph_buf[5];
    glyph_utf8(active ? GLYPH_ICON_DEVICE_DEFAULT : GLYPH_ICON_DEVICE_SELECTABLE, glyph_buf);
    module_box_draw_text_left(cr, OUTER_MARGIN, row->y, row->h, glyph_buf, FONT.heading, false, COLORS.lavender);

    double name_x = OUTER_MARGIN + g_popup.marker_col_w + ROW_INNER_SPACING;
    module_box_draw_text_left(cr, name_x, row->y, row->h, name, FONT.label, active,
                               active ? COLORS.lavender : COLORS.text);

    // Right-aligned description, replaced by a status while applying --
    // reuses draw_text_left with a precomputed x, same trick device_popup.c
    // uses for its own right-aligned percent column.
    const char *right = pending ? "applying..." : PROFILES[i].description;
    double rw, rh;
    module_box_measure_text(right, FONT.sm, false, &rw, &rh);
    (void)rh;
    module_box_draw_text_left(cr, OUTER_MARGIN + g_popup.content_width - rw, row->y, row->h, right, FONT.sm,
                               false, pending ? COLORS.yellow : COLORS.subtext0);
}

static void draw_popup(cairo_t *cr, void *user_data) {
    (void)user_data;
    module_box_draw_rect(cr, 0, 0, g_popup.popup_width, g_popup.popup_height,
                          COLORS.mantle, COLORS.lavender, (double)BAR.module_border_width);
    module_box_draw_text_left(cr, OUTER_MARGIN, g_popup.header_y, g_popup.header_h, g_popup.header,
                               FONT.md, true, COLORS.lavender);
    draw_graph(cr);
    for (int i = 0; i < PROFILE_COUNT; i++) {
        draw_row(cr, i);
    }
}

static void redraw(void) {
    if (g_popup.popup) {
        popup_surface_render_now(g_popup.popup, draw_popup, NULL);
    }
}

void eq_popup_refresh_if_open(void) {
    if (!g_popup.open) {
        return;
    }
    // Only the header text can change with the state (the popup's own size
    // is fixed by GRAPH_W), so no surface resize is needed -- just a rebuild
    // of the header string and a redraw.
    build_header(g_popup.header, sizeof(g_popup.header));
    redraw();
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

void eq_popup_open(Output *output, double box_x, double box_y, double box_w, double box_h) {
    eq_popup_close(); // at most one EQ popup open at a time

    g_popup.output = output;
    g_popup.hovered_index = -1;
    compute_layout();

    // Same TOP+RIGHT anchor+margin math (including the BAR.height
    // correction) as audio/device_popup.c -- see CLAUDE.md's "D-Bus /
    // system tray" section for the reasoning.
    int top_margin = (int)(box_y + box_h) - BAR.height;
    int right_margin = output->width - (int)(box_x + box_w);
    if (right_margin < 0) {
        right_margin = 0;
    }

    g_popup.popup = popup_surface_create(output->app, output, (int)ceil(g_popup.popup_width),
                                          (int)ceil(g_popup.popup_height), top_margin, right_margin, 0, 0,
                                          ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
                                          ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
    if (!g_popup.popup) {
        return;
    }

    create_catchers(output->app);
    g_popup.open = true;
    redraw();
}

void eq_popup_close(void) {
    if (!g_popup.open) {
        return;
    }
    popup_surface_destroy(g_popup.popup);
    g_popup.popup = NULL;
    destroy_catchers();
    g_popup.open = false;
    g_popup.output = NULL;
    g_popup.hovered_index = -1;
}

bool eq_popup_is_menu_surface(struct wl_surface *surface) {
    return g_popup.open && g_popup.popup && popup_surface_wl_surface(g_popup.popup) == surface;
}

bool eq_popup_is_catcher_surface(struct wl_surface *surface) {
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

static int row_at(double x, double y) {
    if (x < OUTER_MARGIN || x >= OUTER_MARGIN + g_popup.content_width) {
        return -1;
    }
    for (int i = 0; i < PROFILE_COUNT; i++) {
        if (y >= g_popup.rows[i].y && y < g_popup.rows[i].y + g_popup.rows[i].h) {
            return i;
        }
    }
    return -1;
}

void eq_popup_handle_motion(double x, double y) {
    if (!g_popup.open) {
        return;
    }
    int idx = row_at(x, y);
    if (idx != g_popup.hovered_index) {
        g_popup.hovered_index = idx;
        redraw();
    }
}

void eq_popup_handle_click(double x, double y) {
    if (!g_popup.open) {
        return;
    }
    int idx = row_at(x, y);
    if (idx < 0) {
        return; // header/graph area -- read-only, absorbed
    }
    // Deliberately stays open (like audio/device_popup.c): the row flips to
    // "applying..." now, and the graph redraws with the new curve once the
    // script's state-file write arrives via main.c's inotify branch.
    eq_state_apply_profile(PROFILES[idx].name);
    redraw();
    // The bar box's own label also switches to "NAME..." while pending.
    output_mark_all_dirty(g_popup.output->app);
}
