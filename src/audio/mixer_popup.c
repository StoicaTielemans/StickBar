#include "audio/mixer_popup.h"

#include <linux/input-event-codes.h> // BTN_LEFT
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio/pulse_client.h"
#include "bar/popup_surface.h"
#include "config/glyphs.h"
#include "render/module_box.h"

// Sized generously (this machine has 2 monitors), matching the same
// MAX_TRACKED_OUTPUTS convention used by audio/device_popup.c.
#define MAX_TRACKED_OUTPUTS 8

// Mirrors audio/pulse_client.c's MAX_PULSE_STREAMS, redeclared locally for the
// same reason device_popup.c redeclares MAX_PULSE_NODES as MAX_DEVICE_ROWS.
#define MAX_MIXER_ROWS 32

// Geometry. Spacing/margins deliberately match audio/device_popup.c's own
// values so the two dropdowns read as siblings; the name/slider widths are
// this popup's own tuning (there's no reference-bar mixer to mirror).
#define ROW_SPACING 8.0
#define ROW_INNER_SPACING 10.0
#define OUTER_MARGIN 8.0
#define NAME_COL_W 150.0
#define SLIDER_W 140.0
#define SLIDER_TRACK_H 4.0
#define SLIDER_KNOB_R 5.0
#define PERCENT_COL_W 40.0
#define MIN_ROW_H 20.0
#define NAME_BUF_SIZE 300
#define PCT_BUF_SIZE 16

// The slider spans 0..100%, not 0..PULSE_MAX_VOLUME_PERCENT -- per-app
// boosting past 100% is rarely what anyone wants from a mixer, so the full
// track width goes to the useful range. A stream already above 100% (set
// elsewhere, or via scrolling, which still allows up to the shared 200% cap)
// just shows a full track; its percent label still reports the real value.
#define SLIDER_MAX_PERCENT 100

#define EMPTY_TEXT "No applications playing audio"

typedef struct {
    double y, h;
} RowLayout;

typedef struct {
    bool open;
    Color accent;
    Output *output;
    PopupSurface *popup;

    PulseStream *streams[MAX_MIXER_ROWS];
    RowLayout rows[MAX_MIXER_ROWS];
    char name_text[MAX_MIXER_ROWS][NAME_BUF_SIZE];
    int row_count;
    int hovered_index;

    // Drag state: the stream being dragged (a pointer, not a row index -- rows
    // can reorder/disappear under a live refresh mid-drag) plus the percent
    // last sent for it, drawn in place of the cached value so the knob tracks
    // the pointer immediately rather than lagging one server round trip.
    PulseStream *drag_stream;
    int drag_percent;

    double mute_col_w, content_width, popup_width, popup_height;

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

// Same UTF-8-boundary-safe binary-search truncation as audio/device_popup.c's
// truncate_to_width() (kept as a local copy, matching how modules/media keeps
// its own copies) -- app names, unlike device descriptions, can genuinely be
// non-ASCII, so the codepoint-boundary handling matters more here.
static void truncate_to_width(const char *text, double max_w, int font_px, bool bold,
                               char *out, size_t out_size) {
    double w, h;
    module_box_measure_text(text, font_px, bold, &w, &h);
    if (w <= max_w) {
        snprintf(out, out_size, "%s", text);
        return;
    }

    size_t len = strlen(text);
    size_t offsets[NAME_BUF_SIZE];
    size_t n = 0;
    for (size_t i = 0; i < len && n < NAME_BUF_SIZE - 1;) {
        offsets[n++] = i;
        unsigned char c = (unsigned char)text[i];
        size_t clen = 1;
        if ((c & 0xE0) == 0xC0) {
            clen = 2;
        } else if ((c & 0xF0) == 0xE0) {
            clen = 3;
        } else if ((c & 0xF8) == 0xF0) {
            clen = 4;
        }
        i += clen;
    }
    offsets[n] = len;

    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        char candidate[NAME_BUF_SIZE];
        snprintf(candidate, sizeof(candidate), "%.*s...", (int)offsets[mid], text);
        module_box_measure_text(candidate, font_px, bold, &w, &h);
        if (w <= max_w) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    snprintf(out, out_size, "%.*s...", (int)offsets[lo], text);
}

// Column x positions, all derived from the fixed column widths -- the popup's
// width never depends on its content, so only its height changes as streams
// come and go (which keeps the right-anchored popup from jumping sideways).
static double name_x(void) { return OUTER_MARGIN; }
static double slider_x(void) { return name_x() + NAME_COL_W + ROW_INNER_SPACING; }
static double percent_x(void) { return slider_x() + SLIDER_W + ROW_INNER_SPACING; }
static double mute_x(void) { return percent_x() + PERCENT_COL_W + ROW_INNER_SPACING; }

// Rebuilt from pulse_client's live cache on open and on every refresh, same
// "nothing to snapshot, just re-read" model as device_popup.c's
// compute_layout().
static void compute_layout(void) {
    char buf[5];
    static const uint32_t ICONS[4] = { GLYPH_ICON_VOLUME_MUTED, GLYPH_ICON_VOLUME_LOW,
                                        GLYPH_ICON_VOLUME_MEDIUM, GLYPH_ICON_VOLUME_HIGH };
    double mute_w = 0, mute_h = 0;
    for (int i = 0; i < 4; i++) {
        glyph_utf8(ICONS[i], buf);
        double w, h;
        module_box_measure_text(buf, FONT.heading, false, &w, &h);
        if (w > mute_w) mute_w = w;
        if (h > mute_h) mute_h = h;
    }
    g_mixer.mute_col_w = mute_w;
    g_mixer.content_width = mute_x() + mute_w - OUTER_MARGIN;
    g_mixer.popup_width = g_mixer.content_width + OUTER_MARGIN * 2.0;

    int count = pulse_stream_count();
    if (count > MAX_MIXER_ROWS) {
        count = MAX_MIXER_ROWS;
    }

    double row_h = mute_h > MIN_ROW_H ? mute_h : MIN_ROW_H;
    int valid = 0;
    for (int i = 0; i < count; i++) {
        PulseStream *stream = pulse_stream_get(i);
        if (!stream) {
            continue; // removed between count() and get() -- the next refresh catches up
        }
        g_mixer.streams[valid] = stream;
        truncate_to_width(pulse_stream_name(stream), NAME_COL_W, FONT.label, false,
                          g_mixer.name_text[valid], NAME_BUF_SIZE);
        valid++;
    }
    g_mixer.row_count = valid;

    double y = OUTER_MARGIN;
    int visual_rows = valid > 0 ? valid : 1; // the empty-state message takes one row
    for (int i = 0; i < visual_rows; i++) {
        g_mixer.rows[i].y = y;
        g_mixer.rows[i].h = row_h;
        y += row_h;
        if (i < visual_rows - 1) {
            y += ROW_SPACING;
        }
    }
    g_mixer.popup_height = y + OUTER_MARGIN;
}

static void draw_transparent(cairo_t *cr, void *user_data) {
    (void)user_data;
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

static void draw_slider(cairo_t *cr, double y, double h, int percent, bool muted) {
    double frac = (double)percent / SLIDER_MAX_PERCENT;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;

    double track_y = y + (h - SLIDER_TRACK_H) / 2.0;
    double sx = slider_x();
    Color fill = muted ? COLORS.overlay0 : g_mixer.accent;

    module_box_draw_rect(cr, sx, track_y, SLIDER_W, SLIDER_TRACK_H,
                          COLORS.surface1, (Color){ 0, 0, 0, 0 }, 0.0);
    module_box_draw_rect(cr, sx, track_y, SLIDER_W * frac, SLIDER_TRACK_H,
                          fill, (Color){ 0, 0, 0, 0 }, 0.0);

    cairo_save(cr);
    cairo_set_source_rgba(cr, fill.r, fill.g, fill.b, fill.a);
    cairo_arc(cr, sx + SLIDER_W * frac, y + h / 2.0, SLIDER_KNOB_R, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_restore(cr);
}

static void draw_row(cairo_t *cr, int i) {
    PulseStream *stream = g_mixer.streams[i];
    RowLayout *row = &g_mixer.rows[i];
    bool muted = pulse_stream_muted(stream);
    int percent = stream == g_mixer.drag_stream ? g_mixer.drag_percent : pulse_stream_volume_percent(stream);

    if (i == g_mixer.hovered_index) {
        module_box_draw_rect(cr, OUTER_MARGIN, row->y, g_mixer.content_width, row->h,
                              COLORS.surface0, (Color){ 0, 0, 0, 0 }, 0.0);
    }

    module_box_draw_text_left(cr, name_x(), row->y, row->h, g_mixer.name_text[i], FONT.label, false,
                               muted ? COLORS.overlay0 : COLORS.text);

    draw_slider(cr, row->y, row->h, percent, muted);

    char pct_text[PCT_BUF_SIZE];
    snprintf(pct_text, sizeof(pct_text), "%d%%", percent);
    double pct_w, pct_h;
    module_box_measure_text(pct_text, FONT.sm, false, &pct_w, &pct_h);
    (void)pct_h;
    // Right-aligned within its column, same trick as device_popup.c.
    module_box_draw_text_left(cr, percent_x() + (PERCENT_COL_W - pct_w), row->y, row->h,
                               pct_text, FONT.sm, false, COLORS.subtext0);

    char glyph_buf[5];
    glyph_utf8(status_icon_glyph(percent, muted), glyph_buf);
    module_box_draw_text_left(cr, mute_x(), row->y, row->h, glyph_buf, FONT.heading, false,
                               muted ? COLORS.overlay0 : g_mixer.accent);
}

static void draw_popup(cairo_t *cr, void *user_data) {
    (void)user_data;
    module_box_draw_rect(cr, 0, 0, g_mixer.popup_width, g_mixer.popup_height,
                          COLORS.mantle, g_mixer.accent, (double)BAR.module_border_width);
    if (g_mixer.row_count == 0) {
        module_box_draw_text_left(cr, OUTER_MARGIN, g_mixer.rows[0].y, g_mixer.rows[0].h,
                                   EMPTY_TEXT, FONT.label, false, COLORS.subtext0);
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
        PopupSurface *catcher = popup_surface_create(
            app, o, 0, 0, 0, 0, 0, 0,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
            ZWLR_LAYER_SHELL_V1_LAYER_TOP);
        if (!catcher) {
            continue;
        }
        popup_surface_render_now(catcher, draw_transparent, NULL);
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
    if (x < OUTER_MARGIN || x >= OUTER_MARGIN + g_mixer.content_width) {
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

// Maps a surface-local x to a slider percent, clamped to the track -- used
// both for the initial press and for every drag motion afterward (where x
// can land well outside the track, or the popup entirely, under the
// implicit grab).
static int percent_at_x(double x) {
    double frac = (x - slider_x()) / SLIDER_W;
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

    if (x >= mute_x()) {
        pulse_stream_toggle_mute(stream);
    } else if (x >= slider_x() - SLIDER_KNOB_R && x < slider_x() + SLIDER_W + SLIDER_KNOB_R) {
        // Small knob-radius slop on both ends so grabbing the knob at exactly
        // 0% or 100% still starts a drag.
        g_mixer.drag_stream = stream;
        g_mixer.drag_percent = -1; // force drag_to() to send the first value
        drag_to(x);
    }
    // Landing on the name or percent column does nothing -- stays open
    // either way, same as device_popup.c.
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
    if (idx != g_mixer.hovered_index) {
        g_mixer.hovered_index = idx;
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
