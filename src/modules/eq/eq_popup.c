#include "modules/eq/eq_popup.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bar/popup_surface.h"
#include "config/glyphs.h"
#include "eq/eq_curve.h"
#include "eq/eq_state.h"
#include "render/color.h"
#include "render/module_box.h"
#include "wayland/output.h"

// Sized generously (this machine has 2 monitors), matching the same
// MAX_TRACKED_OUTPUTS convention used by every other popup in this project.
#define MAX_TRACKED_OUTPUTS 8

// The profiles offered in the dropdown, in display order -- a deliberate
// subset of DacController's presets/ (the user asked for exactly these:
// the IEM profile, the two DT 990 variants worth switching between day to
// day, and a flat EQ at each gain mode -- flat-high/flat-low are presets
// with no bands, so `profile` resets every band to 0 dB and sets the gain,
// unlike the script's own `flat` command, which leaves gain alone). Each
// name must match a presets/NAME.txt file, since
// it's passed straight to `blackpearl-eq.py profile NAME`.
typedef struct {
    const char *name;
    const char *description;
} EqProfileEntry;

static const EqProfileEntry PROFILES[] = {
    { "zero2",      "Zero:2 IEM · low gain" },
    { "dt990",      "DT 990 · full EQ, more bass" },
    { "dt990-loud", "DT 990 · treble fix, louder" },
    { "flat-high",  "Flat EQ · high gain" },
    { "flat-low",   "Flat EQ · low gain" },
};
#define PROFILE_COUNT ((int)(sizeof(PROFILES) / sizeof(PROFILES[0])))

// Layout tuning, local to this popup (see CLAUDE.md "Conventions"). Header,
// pill and card styling match the other dropdowns on purpose. Everything is
// a fixed size (nothing here depends on the state's text), so a state change
// only ever repaints the existing surface -- the size never changes.
#define CONTENT_W 340.0
#define OUTER_MARGIN 12.0
#define SECTION_GAP 10.0
#define HEADER_H 22.0
#define PILL_H 16.0
#define PILL_PAD_X 7.0
#define CARD_PAD 8.0
#define CARD_GAP 6.0
#define GRAPH_LABEL_H 14.0
#define GRAPH_TOP_GAP 8.0      // leaves room for the top dB label, centred on the graph's top edge
#define GRAPH_H 110.0
#define DB_LABEL_W 24.0        // left column for the "+6"/"-6" grid labels
#define FREQ_LABEL_GAP 2.0     // between the graph's bottom edge and the Hz labels
#define AXIS_LABEL_H 12.0
#define SECTION_LABEL_H 14.0
#define SECTION_LABEL_GAP 4.0
#define PROFILE_CARD_H 30.0
#define COL_GAP 10.0
// The graph's dB span: at least +/-12 dB, growing in 6 dB grid steps to fit
// heavier EQs (e.g. the bass-v preset's +19 dB) without clipping them.
#define GRAPH_MIN_RANGE_DB 12.0
#define GRAPH_STEP_DB 6.0

// Derived geometry: a straight stack, top to bottom.
#define POPUP_W (CONTENT_W + OUTER_MARGIN * 2.0)
#define INNER_X (OUTER_MARGIN + CARD_PAD)
#define INNER_W (CONTENT_W - CARD_PAD * 2.0)
#define GRAPH_CARD_Y (OUTER_MARGIN + HEADER_H + SECTION_GAP)
#define GRAPH_CARD_H \
    (CARD_PAD + GRAPH_LABEL_H + GRAPH_TOP_GAP + GRAPH_H + FREQ_LABEL_GAP + AXIS_LABEL_H + CARD_PAD)
#define GRAPH_X (INNER_X + DB_LABEL_W)
#define GRAPH_Y (GRAPH_CARD_Y + CARD_PAD + GRAPH_LABEL_H + GRAPH_TOP_GAP)
#define GRAPH_W (INNER_W - DB_LABEL_W)
#define SECTION_LABEL_Y (GRAPH_CARD_Y + GRAPH_CARD_H + SECTION_GAP)
#define PROFILES_Y (SECTION_LABEL_Y + SECTION_LABEL_H + SECTION_LABEL_GAP)
#define POPUP_H \
    (PROFILES_Y + PROFILE_COUNT * PROFILE_CARD_H + (PROFILE_COUNT - 1) * CARD_GAP + OUTER_MARGIN)

typedef struct {
    bool open;
    Output *output;
    PopupSurface *popup;
    int hovered_index;

    PopupSurface *catchers[MAX_TRACKED_OUTPUTS];
    struct wl_surface *catcher_surfaces[MAX_TRACKED_OUTPUTS];
    int catcher_count;
} EqPopupState;

static EqPopupState g_popup = { .hovered_index = -1 };

static double profile_card_y(int i) {
    return PROFILES_Y + i * (PROFILE_CARD_H + CARD_GAP);
}

// ---- Painting ----------------------------------------------------------------

// The pill: whether the EQ is actually doing anything right now. Applying
// beats the rest, since that's what the user just asked for.
static const char *pill_word(Color *fill) {
    if (eq_state_pending_profile()[0]) {
        *fill = COLORS.yellow;
        return "applying";
    }
    if (!eq_state_available()) {
        *fill = COLORS.overlay1;
        return "no data";
    }
    if (!eq_state_enabled()) {
        *fill = COLORS.overlay1;
        return "off";
    }
    *fill = COLORS.green;
    return "on";
}

static void paint_header(cairo_t *cr) {
    double x = OUTER_MARGIN, y = OUTER_MARGIN;

    Color fill;
    const char *word = pill_word(&fill);
    double pill_w = module_box_text_width(word, FONT.xs, true) + PILL_PAD_X * 2.0;
    double pill_x = x + CONTENT_W - pill_w;
    double pill_y = y + (HEADER_H - PILL_H) / 2.0;

    // Clipped short of the pill: the preset name comes from the state file,
    // so an unusually long one must not draw over it.
    cairo_save(cr);
    cairo_rectangle(cr, x, y, pill_x - COL_GAP - x, HEADER_H);
    cairo_clip(cr);
    char icon[5];
    glyph_utf8(GLYPH_ICON_EQUALIZER, icon);
    module_box_draw_text_left(cr, x, y, HEADER_H, icon, FONT.heading, false, COLORS.lavender);
    double tx = x + module_box_text_width(icon, FONT.heading, false) + 8.0;
    const char *title = !eq_state_available()      ? "No EQ state yet"
                        : eq_state_profile()[0]    ? eq_state_profile()
                                                   : "custom EQ";
    module_box_draw_text_left(cr, tx, y, HEADER_H, title, FONT.md, true, COLORS.text);
    if (eq_state_gain()[0]) {
        char summary[48];
        snprintf(summary, sizeof(summary), "  ·  %s gain", eq_state_gain());
        tx += module_box_text_width(title, FONT.md, true);
        module_box_draw_text_left(cr, tx, y, HEADER_H, summary, FONT.sm, false, COLORS.subtext0);
    }
    cairo_restore(cr);

    module_box_draw_rect(cr, pill_x, pill_y, pill_w, PILL_H, fill, COLORS.mantle, 0.0);
    module_box_draw_text_centered(cr, pill_x, pill_y, pill_w, PILL_H, word, FONT.xs, true, COLORS.mantle);
}

static void stroke_line(cairo_t *cr, double x0, double y0, double x1, double y1, Color c) {
    color_set_source(cr, c);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x0, y0);
    cairo_line_to(cr, x1, y1);
    cairo_stroke(cr);
}

// A `base` card holding a label row (preamp, peak/min), then the dB grid
// (every GRAPH_STEP_DB, 0 dB brighter) with labels on the left, the decade
// frequency grid (100 Hz / 1 kHz / 10 kHz) with labels below, and the curve.
static void paint_graph_card(cairo_t *cr) {
    double gx = GRAPH_X, gy = GRAPH_Y;
    double range = eq_curve_range_db(GRAPH_MIN_RANGE_DB, GRAPH_STEP_DB);
    double mid_y = gy + GRAPH_H / 2.0;
    double px_per_db = (GRAPH_H / 2.0) / range;

    module_box_draw_rect(cr, OUTER_MARGIN, GRAPH_CARD_Y, CONTENT_W, GRAPH_CARD_H, COLORS.base, COLORS.surface1, 1.0);

    double label_y = GRAPH_CARD_Y + CARD_PAD;
    if (eq_state_available()) {
        char preamp[32], extent[48];
        snprintf(preamp, sizeof(preamp), "preamp %+.1f dB", eq_state_preamp());
        snprintf(extent, sizeof(extent), "peak %+.1f  min %+.1f dB", eq_state_peak_db(), eq_state_min_db());
        module_box_draw_text_left(cr, INNER_X, label_y, GRAPH_LABEL_H, preamp, FONT.sm, false, COLORS.subtext0);
        module_box_draw_text_right(cr, INNER_X + INNER_W, label_y, GRAPH_LABEL_H, extent, FONT.xs, false,
                                   COLORS.overlay0);
    } else {
        module_box_draw_text_left(cr, INNER_X, label_y, GRAPH_LABEL_H, "run blackpearl-eq.py state", FONT.sm, false,
                                  COLORS.subtext0);
    }

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
        module_box_draw_text_right(cr, gx - 4.0, y - AXIS_LABEL_H / 2.0, AXIS_LABEL_H, label, FONT.xs, false,
                                   COLORS.overlay0);
    }

    static const double DECADES[] = { 100.0, 1000.0, 10000.0 };
    static const char *DECADE_LABELS[] = { "100", "1k", "10k" };
    for (int i = 0; i < 3; i++) {
        double x = floor(eq_curve_freq_to_x(DECADES[i], gx, GRAPH_W)) + 0.5;
        stroke_line(cr, x, gy, x, gy + GRAPH_H, COLORS.surface0);
        double lw = module_box_text_width(DECADE_LABELS[i], FONT.xs, false);
        module_box_draw_text_left(cr, x - lw / 2.0, gy + GRAPH_H + FREQ_LABEL_GAP, AXIS_LABEL_H, DECADE_LABELS[i],
                                  FONT.xs, false, COLORS.overlay0);
    }

    Color accent = eq_state_enabled() ? COLORS.lavender : COLORS.overlay0;
    eq_curve_draw(cr, gx, gy, GRAPH_W, GRAPH_H, range, accent, 2.0, true, COLORS.surface2);
}

// One card per profile, the same language as the audio device cards: the
// active one has the accent border and name, a hovered one lightens to show
// it's clickable, and a small tag says what a click does or is doing.
static void paint_profile_card(cairo_t *cr, int i) {
    const char *name = PROFILES[i].name;
    bool active = strcmp(eq_state_profile(), name) == 0;
    bool pending = strcmp(eq_state_pending_profile(), name) == 0;
    bool hovered = i == g_popup.hovered_index;
    double y = profile_card_y(i);

    module_box_draw_rect(cr, OUTER_MARGIN, y, CONTENT_W, PROFILE_CARD_H, hovered ? COLORS.surface0 : COLORS.base,
                         active ? COLORS.lavender : COLORS.surface1, active ? 2.0 : 1.0);

    module_box_draw_text_left(cr, INNER_X, y, PROFILE_CARD_H, name, FONT.label, true,
                              active ? COLORS.lavender : COLORS.text);

    const char *tag = pending ? "applying..." : active ? "active" : hovered ? "apply" : "";
    Color tag_color = pending ? COLORS.yellow : active ? COLORS.lavender : COLORS.overlay0;
    if (tag[0]) {
        double tag_x = INNER_X + module_box_text_width(name, FONT.label, true) + COL_GAP;
        module_box_draw_text_left(cr, tag_x, y, PROFILE_CARD_H, tag, FONT.xs, true, tag_color);
    }

    module_box_draw_text_right(cr, INNER_X + INNER_W, y, PROFILE_CARD_H, PROFILES[i].description, FONT.sm, false,
                               COLORS.subtext0);
}

static void draw_popup(cairo_t *cr, void *user_data) {
    (void)user_data;
    module_box_draw_rect(cr, 0, 0, POPUP_W, POPUP_H, COLORS.mantle, COLORS.lavender,
                         (double)BAR.module_border_width);
    paint_header(cr);
    paint_graph_card(cr);
    module_box_draw_text_left(cr, OUTER_MARGIN, SECTION_LABEL_Y, SECTION_LABEL_H, "PROFILES", FONT.xs, true,
                              COLORS.overlay0);
    for (int i = 0; i < PROFILE_COUNT; i++) {
        paint_profile_card(cr, i);
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
    redraw();
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

void eq_popup_open(Output *output, double box_x, double box_y, double box_w, double box_h) {
    eq_popup_close(); // at most one EQ popup open at a time

    g_popup.output = output;
    g_popup.hovered_index = -1;

    // Same TOP+RIGHT anchor+margin math (including the BAR.height
    // correction) as audio/device_popup.c -- see
    // docs/popups.md for the reasoning.
    int top_margin = (int)(box_y + box_h) - BAR.height;
    int right_margin = output->width - (int)(box_x + box_w);
    if (right_margin < 0) {
        right_margin = 0;
    }

    g_popup.popup = popup_surface_create(output->app, output, (int)POPUP_W, (int)POPUP_H, top_margin, right_margin, 0, 0,
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

static int card_at(double x, double y) {
    if (x < OUTER_MARGIN || x >= OUTER_MARGIN + CONTENT_W) {
        return -1;
    }
    for (int i = 0; i < PROFILE_COUNT; i++) {
        double card_y = profile_card_y(i);
        if (y >= card_y && y < card_y + PROFILE_CARD_H) {
            return i;
        }
    }
    return -1;
}

void eq_popup_handle_motion(double x, double y) {
    if (!g_popup.open) {
        return;
    }
    int idx = card_at(x, y);
    if (idx != g_popup.hovered_index) {
        g_popup.hovered_index = idx;
        redraw();
    }
}

void eq_popup_handle_click(double x, double y) {
    if (!g_popup.open) {
        return;
    }
    int idx = card_at(x, y);
    if (idx < 0) {
        return; // header/graph area -- read-only, absorbed
    }
    // Deliberately stays open (like audio/device_popup.c): the card and the
    // pill flip to "applying" now, and the graph redraws with the new curve once the
    // script's state-file write arrives via main.c's inotify branch.
    eq_state_apply_profile(PROFILES[idx].name);
    redraw();
    // The bar box's own label also switches to "NAME..." while pending.
    output_mark_all_dirty(g_popup.output->app);
}

// ---- Test-control hooks (testing/test_control.c) ---------------------------------

int eq_popup_test_card_count(void) { return g_popup.open ? PROFILE_COUNT : -1; }

bool eq_popup_test_target(const char *profile, double *x, double *y) {
    if (!g_popup.open) {
        return false;
    }
    for (int i = 0; i < PROFILE_COUNT; i++) {
        if (strcmp(PROFILES[i].name, profile) == 0) {
            *x = OUTER_MARGIN + CONTENT_W / 2.0;
            *y = profile_card_y(i) + PROFILE_CARD_H / 2.0;
            return true;
        }
    }
    return false;
}
