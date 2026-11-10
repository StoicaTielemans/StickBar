#include "audio/mixer_popup.h"

#include <linux/input-event-codes.h> // BTN_LEFT
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio/pulse_client.h"
#include "bar/popup_surface.h"
#include "config/glyphs.h"
#include "render/meter_bar.h"
#include "render/module_box.h"

// Sized generously (this machine has 2 monitors), matching the same
// MAX_TRACKED_OUTPUTS convention used by audio/device_popup.c.
#define MAX_TRACKED_OUTPUTS 8

// Mirrors audio/pulse_client.c's MAX_PULSE_STREAMS, redeclared locally for the
// same reason device_popup.c redeclares MAX_PULSE_NODES as MAX_DEVICE_ROWS.
#define MAX_MIXER_ROWS 32

// Layout tuning, local to this popup. Deliberately the same width, header,
// card and chip styling as audio/device_popup.c so the two dropdowns read as
// siblings. The width never depends on content, so only the height changes
// as streams come and go (which keeps the right-anchored popup from jumping
// sideways).
#define CONTENT_W 340.0
#define OUTER_MARGIN 12.0
#define SECTION_GAP 10.0
#define CARD_GAP 8.0
#define HEADER_H 22.0
#define PILL_H 16.0
#define PILL_PAD_X 7.0
#define CARD_PAD 10.0
#define CHIP_W 30.0
#define CHIP_H 22.0
#define SLIDER_GAP 6.0
#define SLIDER_ROW_H 12.0 // hit area of the slider row; the meter itself is METER_H tall
#define METER_H 6.0
#define METER_SEGMENTS 36
#define METER_SLOP 4.0 // extra grab room past both slider ends
#define COL_GAP 10.0
#define EMPTY_CARD_H 40.0
#define NAME_BUF_SIZE 300

// The slider spans 0..100%, not 0..PULSE_MAX_VOLUME_PERCENT -- per-app
// boosting past 100% is rarely what anyone wants from a mixer, so the full
// track width goes to the useful range. A stream already above 100% (set
// elsewhere, or via scrolling, which still allows up to the shared 200% cap)
// shows a full track with a peach top end; its percent still reports the
// real value.
#define SLIDER_MAX_PERCENT 100

#define EMPTY_TEXT "No applications playing audio"

typedef struct {
    double y, h; // card rect, relative to the popup (x is always OUTER_MARGIN)
} RowLayout;

typedef struct {
    bool open;
    Color accent;
    Output *output;
    PopupSurface *popup;

    PulseStream *streams[MAX_MIXER_ROWS];
    RowLayout rows[MAX_MIXER_ROWS];
    int row_count;
    int hovered_index;
    bool hovered_mute; // hovering the hovered row's mute chip

    // Drag state: the stream being dragged (a pointer, not a row index -- rows
    // can reorder/disappear under a live refresh mid-drag) plus the percent
    // last sent for it, drawn in place of the cached value so the slider
    // tracks the pointer immediately rather than lagging one server round trip.
    PulseStream *drag_stream;
    int drag_percent;

    double popup_width, popup_height;

    // Kept from open time so the menu surface can be recreated at a new
    // height on a live row-count change (see mixer_popup_refresh_if_open()).
    int top_margin, right_margin;

    PopupSurface *catchers[MAX_TRACKED_OUTPUTS];
    struct wl_surface *catcher_surfaces[MAX_TRACKED_OUTPUTS];
    int catcher_count;
} MixerPopupState;

static MixerPopupState g_mixer = { .hovered_index = -1 };

// Same 4-level thresholds as modules/volume's own status icon.
static uint32_t status_icon_glyph(int percent, bool muted) {
    if (muted || percent == 0) return GLYPH_ICON_VOLUME_MUTED;
    if (percent < 34) return GLYPH_ICON_VOLUME_LOW;
    if (percent < 67) return GLYPH_ICON_VOLUME_MEDIUM;
    return GLYPH_ICON_VOLUME_HIGH;
}

// ---- Card geometry ---------------------------------------------------------------
// Each card: a top row (name ... percent [mute chip]) and a full-width
// slider under it. Every card has the same shape, so x positions are shared.

static double card_h(void) { return CARD_PAD + CHIP_H + SLIDER_GAP + SLIDER_ROW_H + CARD_PAD; }
static double inner_x(void) { return OUTER_MARGIN + CARD_PAD; }
static double inner_w(void) { return CONTENT_W - CARD_PAD * 2.0; }
static double mute_x(void) { return inner_x() + inner_w() - CHIP_W; }
static double percent_col_w(void) { return module_box_text_width("200%", FONT.sm, false); }
static double slider_x(void) { return inner_x(); }
static double slider_w(void) { return inner_w(); }
static double slider_row_y(const RowLayout *row) { return row->y + CARD_PAD + CHIP_H + SLIDER_GAP; }

// Rebuilt from pulse_client's live cache on open and on every refresh, same
// "nothing to snapshot, just re-read" model as device_popup.c's
// compute_layout().
static void compute_layout(void) {
    int count = pulse_stream_count();
    if (count > MAX_MIXER_ROWS) {
        count = MAX_MIXER_ROWS;
    }

    int valid = 0;
    for (int i = 0; i < count; i++) {
        PulseStream *stream = pulse_stream_get(i);
        if (!stream) {
            continue; // removed between count() and get() -- the next refresh catches up
        }
        g_mixer.streams[valid++] = stream;
    }
    g_mixer.row_count = valid;

    double y = OUTER_MARGIN + HEADER_H + SECTION_GAP;
    if (valid == 0) {
        g_mixer.rows[0].y = y; // the empty-state card
        g_mixer.rows[0].h = EMPTY_CARD_H;
        y += EMPTY_CARD_H;
    }
    for (int i = 0; i < valid; i++) {
        if (i > 0) {
            y += CARD_GAP;
        }
        g_mixer.rows[i].y = y;
        g_mixer.rows[i].h = card_h();
        y += card_h();
    }
    g_mixer.popup_width = CONTENT_W + OUTER_MARGIN * 2.0;
    g_mixer.popup_height = y + OUTER_MARGIN;
}

// ---- Painting ----------------------------------------------------------------

// "Mixer · 3 apps · TE-C Analog Stereo" plus a pill with the default sink's
// volume (or "muted") -- the device these streams mostly play through.
static void paint_header(cairo_t *cr) {
    double x = OUTER_MARGIN, y = OUTER_MARGIN;
    const char *sink_name = "";
    for (int i = 0; i < pulse_sink_count(); i++) {
        PulseNode *node = pulse_sink_get(i);
        if (node && pulse_node_is_default(node)) {
            sink_name = pulse_node_description(node);
            break;
        }
    }

    char word[16];
    bool muted = pulse_default_sink_muted();
    if (muted) {
        snprintf(word, sizeof(word), "muted");
    } else {
        snprintf(word, sizeof(word), "%d%%", pulse_default_sink_volume_percent());
    }
    double pill_w = module_box_text_width(word, FONT.xs, true) + PILL_PAD_X * 2.0;

    char icon[5], summary[NAME_BUF_SIZE], summary_full[NAME_BUF_SIZE];
    glyph_utf8(GLYPH_ICON_VOLUME_HIGH, icon);
    snprintf(summary_full, sizeof(summary_full), "  ·  %d app%s%s%s", g_mixer.row_count,
             g_mixer.row_count == 1 ? "" : "s", sink_name[0] ? "  ·  " : "", sink_name);
    module_box_draw_text_left(cr, x, y, HEADER_H, icon, FONT.heading, false, g_mixer.accent);
    double tx = x + module_box_text_width(icon, FONT.heading, false) + 8.0;
    module_box_draw_text_left(cr, tx, y, HEADER_H, "Mixer", FONT.md, true, COLORS.text);
    tx += module_box_text_width("Mixer", FONT.md, true);
    module_box_truncate_to_width(summary_full, x + CONTENT_W - pill_w - COL_GAP - tx, FONT.sm, false, summary,
                                 sizeof(summary));
    module_box_draw_text_left(cr, tx, y, HEADER_H, summary, FONT.sm, false, COLORS.subtext0);

    Color fill = muted ? COLORS.red : g_mixer.accent;
    double pill_x = x + CONTENT_W - pill_w;
    double pill_y = y + (HEADER_H - PILL_H) / 2.0;
    module_box_draw_rect(cr, pill_x, pill_y, pill_w, PILL_H, fill, COLORS.mantle, 0.0);
    module_box_draw_text_centered(cr, pill_x, pill_y, pill_w, PILL_H, word, FONT.xs, true, COLORS.mantle);
}

static void draw_row(cairo_t *cr, int i) {
    PulseStream *stream = g_mixer.streams[i];
    RowLayout *row = &g_mixer.rows[i];
    bool muted = pulse_stream_muted(stream);
    int percent = stream == g_mixer.drag_stream ? g_mixer.drag_percent : pulse_stream_volume_percent(stream);
    bool hovered = i == g_mixer.hovered_index;

    module_box_draw_rect(cr, OUTER_MARGIN, row->y, CONTENT_W, row->h, COLORS.base,
                         hovered ? COLORS.overlay0 : COLORS.surface1, 1.0);

    double y = row->y + CARD_PAD;
    char name[NAME_BUF_SIZE];
    double name_max_w = mute_x() - COL_GAP - percent_col_w() - COL_GAP - inner_x();
    module_box_truncate_to_width(pulse_stream_name(stream), name_max_w, FONT.label, true, name, sizeof(name));
    module_box_draw_text_left(cr, inner_x(), y, CHIP_H, name, FONT.label, true,
                              muted ? COLORS.overlay0 : COLORS.text);

    char pct_text[16];
    snprintf(pct_text, sizeof(pct_text), "%d%%", percent);
    module_box_draw_text_right(cr, mute_x() - COL_GAP, y, CHIP_H, pct_text, FONT.sm, false,
                               muted ? COLORS.overlay0 : COLORS.subtext0);

    bool chip_hovered = hovered && g_mixer.hovered_mute;
    module_box_draw_rect(cr, mute_x(), y, CHIP_W, CHIP_H, chip_hovered ? COLORS.surface1 : COLORS.surface0,
                         COLORS.mantle, 0.0);
    char glyph_buf[5];
    glyph_utf8(status_icon_glyph(percent, muted), glyph_buf);
    module_box_draw_text_centered(cr, mute_x(), y, CHIP_W, CHIP_H, glyph_buf, FONT.heading, false,
                                  muted ? COLORS.overlay0 : g_mixer.accent);

    Color gradient[3];
    if (muted) {
        gradient[0] = gradient[1] = gradient[2] = COLORS.overlay0;
    } else {
        gradient[0] = gradient[1] = g_mixer.accent;
        gradient[2] = percent > SLIDER_MAX_PERCENT ? COLORS.peach : g_mixer.accent;
    }
    meter_bar_draw(cr, slider_x(), slider_row_y(row) + (SLIDER_ROW_H - METER_H) / 2.0, slider_w(), METER_H,
                   (double)percent / SLIDER_MAX_PERCENT, METER_SEGMENTS, gradient, COLORS.surface0);
}

static void draw_popup(cairo_t *cr, void *user_data) {
    (void)user_data;
    module_box_draw_rect(cr, 0, 0, g_mixer.popup_width, g_mixer.popup_height,
                          COLORS.mantle, g_mixer.accent, (double)BAR.module_border_width);
    paint_header(cr);
    if (g_mixer.row_count == 0) {
        RowLayout *row = &g_mixer.rows[0];
        module_box_draw_rect(cr, OUTER_MARGIN, row->y, CONTENT_W, row->h, COLORS.base, COLORS.surface1, 1.0);
        module_box_draw_text_left(cr, inner_x(), row->y, row->h, EMPTY_TEXT, FONT.label, false, COLORS.subtext0);
        return;
    }
    for (int i = 0; i < g_mixer.row_count; i++) {
        draw_row(cr, i);
    }
}

static void redraw(void) {
    if (g_mixer.popup) {
        popup_surface_render_now(g_mixer.popup, draw_popup, NULL);
    }
}

static PopupSurface *create_menu_surface(void) {
    return popup_surface_create(g_mixer.output->app, g_mixer.output,
                                (int)g_mixer.popup_width, (int)g_mixer.popup_height,
                                g_mixer.top_margin, g_mixer.right_margin, 0, 0,
                                ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
                                ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
}

void mixer_popup_refresh_if_open(void) {
    if (!g_mixer.open) {
        return;
    }
    double old_h = g_mixer.popup_height;
    compute_layout();
    if (g_mixer.hovered_index >= g_mixer.row_count) {
        g_mixer.hovered_index = -1;
        g_mixer.hovered_mute = false;
    }
    if (g_mixer.popup_height != old_h) {
        // An app started/stopped playing -- the popup needs a new height.
        // Recreated rather than resized in place: a set_size() + commit on
        // the existing layer surface was tried first and Hyprland unmapped
        // the popup on every shrink (observed live, no `closed` event, no
        // protocol error -- it just vanished), while creating a fresh
        // surface is the same path every popup open already exercises. Any
        // slider drag in progress loses its implicit grab here; the next
        // button release still ends it cleanly.
        popup_surface_destroy(g_mixer.popup);
        g_mixer.popup = create_menu_surface();
        if (!g_mixer.popup) {
            mixer_popup_close();
            return;
        }
    }
    redraw();
}

static void create_catchers(AppContext *app) {
    g_mixer.catcher_count = 0;
    for (Output *o = app->outputs; o && g_mixer.catcher_count < MAX_TRACKED_OUTPUTS; o = o->next) {
        PopupSurface *catcher = popup_surface_create_catcher(app, o);
        if (!catcher) {
            continue;
        }
        g_mixer.catchers[g_mixer.catcher_count] = catcher;
        g_mixer.catcher_surfaces[g_mixer.catcher_count] = popup_surface_wl_surface(catcher);
        g_mixer.catcher_count++;
    }
}

static void destroy_catchers(void) {
    for (int i = 0; i < g_mixer.catcher_count; i++) {
        popup_surface_destroy(g_mixer.catchers[i]);
        g_mixer.catchers[i] = NULL;
        g_mixer.catcher_surfaces[i] = NULL;
    }
    g_mixer.catcher_count = 0;
}

void mixer_popup_open(Output *output, Color accent,
                       double box_x, double box_y, double box_w, double box_h) {
    mixer_popup_close();

    g_mixer.accent = accent;
    g_mixer.hovered_index = -1;
    g_mixer.hovered_mute = false;
    g_mixer.drag_stream = NULL;
    g_mixer.output = output;

    // Unlike device_popup_open(), opens even with zero rows -- an explicit
    // "nothing playing" message is more useful feedback for a middle-click
    // than the click appearing to do nothing at all.
    compute_layout();

    // Same right-anchored anchor+margin math (including the BAR.height
    // correction) as audio/device_popup.c -- see modules/tray/
    // tray_context_menu.c for the reasoning behind that correction.
    g_mixer.top_margin = (int)(box_y + box_h) - BAR.height;
    g_mixer.right_margin = output->width - (int)(box_x + box_w);
    if (g_mixer.right_margin < 0) {
        g_mixer.right_margin = 0;
    }

    g_mixer.popup = create_menu_surface();
    if (!g_mixer.popup) {
        return;
    }

    create_catchers(output->app);
    g_mixer.open = true;
    redraw();
}

void mixer_popup_close(void) {
    if (!g_mixer.open) {
        return;
    }
    popup_surface_destroy(g_mixer.popup);
    g_mixer.popup = NULL;
    destroy_catchers();
    g_mixer.open = false;
    g_mixer.output = NULL;
    g_mixer.row_count = 0;
    g_mixer.hovered_index = -1;
    g_mixer.hovered_mute = false;
    g_mixer.drag_stream = NULL;
}

bool mixer_popup_is_menu_surface(struct wl_surface *surface) {
    return g_mixer.open && g_mixer.popup && popup_surface_wl_surface(g_mixer.popup) == surface;
}

bool mixer_popup_is_catcher_surface(struct wl_surface *surface) {
    if (!g_mixer.open) {
        return false;
    }
    for (int i = 0; i < g_mixer.catcher_count; i++) {
        if (g_mixer.catcher_surfaces[i] == surface) {
            return true;
        }
    }
    return false;
}

static int row_at(double x, double y) {
    if (x < OUTER_MARGIN || x >= OUTER_MARGIN + CONTENT_W) {
        return -1;
    }
    for (int i = 0; i < g_mixer.row_count; i++) {
        RowLayout *row = &g_mixer.rows[i];
        if (y >= row->y && y < row->y + row->h) {
            return i;
        }
    }
    return -1;
}

static bool on_mute_chip(const RowLayout *row, double x, double y) {
    double cy = row->y + CARD_PAD;
    return x >= mute_x() && x < mute_x() + CHIP_W && y >= cy && y < cy + CHIP_H;
}

static bool on_slider(const RowLayout *row, double x, double y) {
    double sy = slider_row_y(row);
    return y >= sy && y < sy + SLIDER_ROW_H && x >= slider_x() - METER_SLOP &&
           x < slider_x() + slider_w() + METER_SLOP;
}

// Maps a surface-local x to a slider percent, clamped to the track -- used
// both for the initial press and for every drag motion afterward (where x
// can land well outside the track, or the popup entirely, under the
// implicit grab).
static int percent_at_x(double x) {
    double frac = (x - slider_x()) / slider_w();
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    return (int)lround(frac * SLIDER_MAX_PERCENT);
}

static void drag_to(double x) {
    int pct = percent_at_x(x);
    if (pct == g_mixer.drag_percent) {
        return; // don't flood the server with identical requests on every pixel
    }
    g_mixer.drag_percent = pct;
    pulse_stream_set_volume_percent(g_mixer.drag_stream, pct);
    redraw();
}

void mixer_popup_handle_press(double x, double y, uint32_t button) {
    if (!g_mixer.open || button != BTN_LEFT) {
        return;
    }
    int idx = row_at(x, y);
    if (idx < 0) {
        return;
    }
    PulseStream *stream = g_mixer.streams[idx];
    RowLayout *row = &g_mixer.rows[idx];

    if (on_mute_chip(row, x, y)) {
        pulse_stream_toggle_mute(stream);
    } else if (on_slider(row, x, y)) {
        g_mixer.drag_stream = stream;
        g_mixer.drag_percent = -1; // force drag_to() to send the first value
        drag_to(x);
    }
    // Landing on the name or percent does nothing -- stays open either way,
    // same as device_popup.c.
}

void mixer_popup_handle_motion(double x, double y) {
    if (!g_mixer.open) {
        return;
    }
    if (g_mixer.drag_stream) {
        drag_to(x);
        return;
    }
    int idx = row_at(x, y);
    bool mute = idx >= 0 && on_mute_chip(&g_mixer.rows[idx], x, y);
    if (idx != g_mixer.hovered_index || mute != g_mixer.hovered_mute) {
        g_mixer.hovered_index = idx;
        g_mixer.hovered_mute = mute;
        redraw();
    }
}

void mixer_popup_handle_release(void) {
    if (!g_mixer.drag_stream) {
        return;
    }
    g_mixer.drag_stream = NULL;
    redraw(); // fall back to drawing the cached (server-confirmed) value
}

void mixer_popup_handle_scroll(double x, double y, int direction) {
    if (!g_mixer.open || g_mixer.drag_stream) {
        return;
    }
    int idx = row_at(x, y);
    if (idx < 0) {
        return;
    }
    pulse_stream_adjust_volume(g_mixer.streams[idx],
                               direction > 0 ? PULSE_VOLUME_STEP_PERCENT : -PULSE_VOLUME_STEP_PERCENT);
}

// ---- Test-control hooks (testing/test_control.c) ---------------------------------

int mixer_popup_test_row_count(void) { return g_mixer.open ? g_mixer.row_count : -1; }

bool mixer_popup_test_target(const char *stream_name, const char *part, double frac, double *x, double *y) {
    if (!g_mixer.open) {
        return false;
    }
    for (int i = 0; i < g_mixer.row_count; i++) {
        if (strcmp(pulse_stream_name(g_mixer.streams[i]), stream_name) != 0) {
            continue;
        }
        RowLayout *row = &g_mixer.rows[i];
        if (strcmp(part, "mute") == 0) {
            *x = mute_x() + CHIP_W / 2.0;
            *y = row->y + CARD_PAD + CHIP_H / 2.0;
        } else { // "slider"
            *x = slider_x() + frac * slider_w();
            *y = slider_row_y(row) + SLIDER_ROW_H / 2.0;
        }
        return true;
    }
    return false;
}
