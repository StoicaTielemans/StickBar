#include "modules/media/media_popup.h"

#include <stdio.h>
#include <string.h>

#include "bar/popup_surface.h"
#include "config/glyphs.h"
#include "modules/media/media.h"
#include "render/module_box.h"

// Sized generously (this machine has 2 monitors), matching the same
// MAX_TRACKED_OUTPUTS convention used by modules/tray/tray_context_menu.c
// and audio/device_popup.c.
#define MAX_TRACKED_OUTPUTS 8

// Mirrors mpris/mpris_client.c's MAX_MPRIS_PLAYERS, but that constant isn't
// part of mpris_client.h's public surface -- redeclared locally at the same
// value, same reasoning audio/device_popup.c already gives for its own
// MAX_DEVICE_ROWS redeclaration.
#define MAX_MEDIA_ROWS 16

#define ROW_SPACING 8.0        // matches audio/device_popup.c's ColumnLayout spacing
#define ROW_INNER_SPACING 10.0 // matches each row's RowLayout spacing
#define OUTER_MARGIN 8.0       // matches Config.bar.popupPadding
#define TRACK_TEXT_MAX_W 260.0 // matches device_popup.c's own DESC_MAX_W
#define TRACK_BUF_SIZE 300
#define TIME_BUF_SIZE 16

// Thumbnail slot -- deliberately smaller than the reference MediaPopup.qml's
// own 56x56 (`Image { Layout.preferredWidth: 56 }`), since this bar's own
// module boxes/popups everywhere else run noticeably more compact than the
// reference's Qt-driven sizing.
#define THUMB_SIZE 40.0

// Timeline (progress bar) geometry, read-only -- no click-to-seek in this
// pass, see media_popup_handle_click()'s own doc comment for why.
#define TIMELINE_W 160.0
#define TIMELINE_H 4.0
#define TIME_ROW_GAP 4.0

// Matches the reference MediaPopup.qml's own maxSaneDuration: some players
// (e.g. a browser tab playing a livestream) report a bogus sentinel length
// instead of correctly reporting "no known length" -- treat anything past
// 24h as "no duration" rather than showing a nonsense timeline.
#define MAX_SANE_DURATION_US (86400LL * 1000000LL)

typedef struct {
    double y, h; // whole card: y relative to popup top, h = full card height (x is always OUTER_MARGIN)

    double controls_y, controls_h; // prev/playpause/next + track-text sub-row

    bool has_time; // false if the player reported no (sane) track length
    double time_y, time_h;
    char time_left[TIME_BUF_SIZE], time_right[TIME_BUF_SIZE];
    double progress_frac; // 0..1, only meaningful if has_time
} RowLayout;

typedef struct {
    bool open;
    Color accent;
    Output *output;
    PopupSurface *popup;

    MprisPlayer *players[MAX_MEDIA_ROWS];
    RowLayout rows[MAX_MEDIA_ROWS];
    char track_text[MAX_MEDIA_ROWS][TRACK_BUF_SIZE]; // pre-truncated, computed at layout time
    int row_count;
    int hovered_index;

    double prev_col_w, playpause_col_w, next_col_w, time_col_w, content_width, popup_width, popup_height;

    PopupSurface *catchers[MAX_TRACKED_OUTPUTS];
    struct wl_surface *catcher_surfaces[MAX_TRACKED_OUTPUTS];
    int catcher_count;
} MediaPopupState;

static MediaPopupState g_popup = { .hovered_index = -1 };

// Byte-based binary-search ellipsis truncation -- see modules/media/media.c's
// own copy of this same algorithm (and audio/device_popup.c's original) for
// why it's duplicated locally rather than shared.
static void truncate_to_width(const char *text, double max_w, int font_px, bool bold, char *out, size_t out_size) {
    double w, h;
    module_box_measure_text(text, font_px, bold, &w, &h);
    if (w <= max_w) {
        snprintf(out, out_size, "%s", text);
        return;
    }

    size_t len = strlen(text);
    size_t lo = 0, hi = len;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        char candidate[TRACK_BUF_SIZE];
        snprintf(candidate, sizeof(candidate), "%.*s...", (int)mid, text);
        module_box_measure_text(candidate, font_px, bold, &w, &h);
        if (w <= max_w) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    snprintf(out, out_size, "%.*s...", (int)lo, text);
}

// "m:ss" -- matches MediaPopup.qml's own formatTime() exactly (no leading
// zero on minutes, always two digits on seconds).
static void format_time(int64_t us, char *out, size_t out_size) {
    if (us < 0) {
        us = 0;
    }
    long long total_seconds = (long long)(us / 1000000);
    long long m = total_seconds / 60;
    long long s = total_seconds % 60;
    snprintf(out, out_size, "%lld:%02lld", m, s);
}

// Every row shares the same column x-positions (the widest glyph/text across
// all rows decides each column's width -- see compute_layout()), so these
// are plain functions of shared state rather than per-row fields. The
// thumbnail column is reserved at a fixed offset regardless of whether any
// given row actually has usable art, so columns stay aligned across rows.
static double content_x(void) { return OUTER_MARGIN + THUMB_SIZE + ROW_INNER_SPACING; }
static double control_prev_x(void) { return content_x(); }
static double control_playpause_x(void) { return control_prev_x() + g_popup.prev_col_w + ROW_INNER_SPACING; }
static double control_next_x(void) { return control_playpause_x() + g_popup.playpause_col_w + ROW_INNER_SPACING; }
static double control_track_x(void) { return control_next_x() + g_popup.next_col_w + ROW_INNER_SPACING; }
static double time_left_x(void) { return content_x(); }
static double timeline_x(void) { return time_left_x() + g_popup.time_col_w + ROW_INNER_SPACING; }
static double time_right_x(void) { return timeline_x() + TIMELINE_W + ROW_INNER_SPACING; }

// Recomputes row geometry (and the truncated track-text/time strings drawn
// from) directly from mpris_client's live cache -- same reasoning as
// audio/device_popup.c's compute_layout(): the cache is already live and
// always-current, so there's nothing to snapshot, and this can safely be
// re-run on every open *and* every live update while open (including once a
// second while open, from media_popup_tick() -- see that function's own doc
// comment for why position needs its own polling).
static void compute_layout(void) {
    char buf[5];
    glyph_utf8(GLYPH_ICON_MEDIA_PREV, buf);
    double prev_w, prev_h;
    module_box_measure_text(buf, FONT.heading, false, &prev_w, &prev_h);

    glyph_utf8(GLYPH_ICON_MEDIA_PLAY, buf);
    double play_w, play_h;
    module_box_measure_text(buf, FONT.heading, false, &play_w, &play_h);
    glyph_utf8(GLYPH_ICON_MEDIA_PAUSE, buf);
    double pause_w, pause_h;
    module_box_measure_text(buf, FONT.heading, false, &pause_w, &pause_h);
    double playpause_w = play_w > pause_w ? play_w : pause_w;
    double playpause_h = play_h > pause_h ? play_h : pause_h;

    glyph_utf8(GLYPH_ICON_MEDIA_NEXT, buf);
    double next_w, next_h;
    module_box_measure_text(buf, FONT.heading, false, &next_w, &next_h);

    // "00:00" as a representative sample -- fixed-width enough in practice
    // (this project's own font is monospace) that the column doesn't visibly
    // jitter as the displayed seconds tick over, without needing to measure
    // every row's actual time strings just to find the widest.
    double time_w, time_h;
    module_box_measure_text("00:00", FONT.sm, false, &time_w, &time_h);

    g_popup.prev_col_w = prev_w;
    g_popup.playpause_col_w = playpause_w;
    g_popup.next_col_w = next_w;
    g_popup.time_col_w = time_w;

    int count = mpris_player_count();
    if (count > MAX_MEDIA_ROWS) {
        count = MAX_MEDIA_ROWS; // documented limitation, matches sni_registry's own cap behavior
    }
    g_popup.row_count = count;

    double max_content_w = 0;
    double card_heights[MAX_MEDIA_ROWS];

    for (int i = 0; i < count; i++) {
        MprisPlayer *player = mpris_player_get(i);
        g_popup.players[i] = player;

        char full_text[TRACK_BUF_SIZE];
        media_build_track_text(player, full_text, sizeof(full_text));
        truncate_to_width(full_text, TRACK_TEXT_MAX_W, FONT.label, false, g_popup.track_text[i], TRACK_BUF_SIZE);

        double text_w, text_h;
        module_box_measure_text(g_popup.track_text[i], FONT.label, false, &text_w, &text_h);

        double controls_row_w =
            prev_w + ROW_INNER_SPACING + playpause_w + ROW_INNER_SPACING + next_w + ROW_INNER_SPACING + text_w;
        double controls_row_h = prev_h;
        if (playpause_h > controls_row_h) controls_row_h = playpause_h;
        if (next_h > controls_row_h) controls_row_h = next_h;
        if (text_h > controls_row_h) controls_row_h = text_h;
        g_popup.rows[i].controls_h = controls_row_h;

        int64_t length_us = mpris_player_length_us(player);
        bool has_time = length_us > 0 && length_us < MAX_SANE_DURATION_US;
        g_popup.rows[i].has_time = has_time;

        double time_row_w = 0, time_row_h = 0;
        if (has_time) {
            int64_t pos_us = mpris_player_position_us(player);
            if (pos_us < 0) pos_us = 0;
            if (pos_us > length_us) pos_us = length_us;
            format_time(pos_us, g_popup.rows[i].time_left, TIME_BUF_SIZE);
            format_time(length_us, g_popup.rows[i].time_right, TIME_BUF_SIZE);
            g_popup.rows[i].progress_frac = (double)pos_us / (double)length_us;
            time_row_w = time_w + ROW_INNER_SPACING + TIMELINE_W + ROW_INNER_SPACING + time_w;
            time_row_h = time_h > TIMELINE_H ? time_h : TIMELINE_H;
        }
        g_popup.rows[i].time_h = time_row_h;

        double row_content_w = controls_row_w > time_row_w ? controls_row_w : time_row_w;
        if (row_content_w > max_content_w) {
            max_content_w = row_content_w;
        }

        double card_h = controls_row_h;
        if (has_time) {
            card_h += TIME_ROW_GAP + time_row_h;
        }
        if (card_h < THUMB_SIZE) {
            card_h = THUMB_SIZE; // thumbnail column reserves at least THUMB_SIZE of vertical room
        }
        card_heights[i] = card_h;
        g_popup.rows[i].h = card_h;
    }

    g_popup.content_width = THUMB_SIZE + ROW_INNER_SPACING + max_content_w;
    g_popup.popup_width = g_popup.content_width + OUTER_MARGIN * 2.0;

    double y = OUTER_MARGIN;
    for (int i = 0; i < count; i++) {
        g_popup.rows[i].y = y;
        // Both text sub-rows top-align at the card's own top -- if the
        // thumbnail is taller than the controls+time stack, there's just
        // extra blank space below the text, which is close enough to the
        // reference's own uneven-per-card layout for this popup's purposes
        // without replicating Qt's row-height negotiation.
        g_popup.rows[i].controls_y = y;
        if (g_popup.rows[i].has_time) {
            g_popup.rows[i].time_y = y + g_popup.rows[i].controls_h + TIME_ROW_GAP;
        }
        y += card_heights[i];
        if (i < count - 1) {
            y += ROW_SPACING;
        }
    }
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

static void draw_thumbnail(cairo_t *cr, MprisPlayer *player, RowLayout *row) {
    double thumb_x = OUTER_MARGIN;
    double thumb_y = row->y + (row->h - THUMB_SIZE) / 2.0;

    cairo_surface_t *art = mpris_player_art_surface(player);
    if (!art) {
        // No usable art (no mpris:artUrl reported, an unsupported URL
        // scheme, or decoding failed) -- an explicit placeholder square
        // rather than leaving a blank gap, same "explicit unavailable
        // state" instinct this project's other modules already use for
        // missing data (e.g. the GPU module's "--" when no NVIDIA GPU is
        // present).
        module_box_draw_rect(cr, thumb_x, thumb_y, THUMB_SIZE, THUMB_SIZE, COLORS.surface0, COLORS.overlay0, 1.0);
        return;
    }

    int art_w = cairo_image_surface_get_width(art);
    int art_h = cairo_image_surface_get_height(art);
    cairo_save(cr);
    cairo_rectangle(cr, thumb_x, thumb_y, THUMB_SIZE, THUMB_SIZE);
    cairo_clip(cr);
    cairo_translate(cr, thumb_x, thumb_y);
    if (art_w > 0 && art_h > 0) {
        cairo_scale(cr, THUMB_SIZE / (double)art_w, THUMB_SIZE / (double)art_h);
    }
    cairo_set_source_surface(cr, art, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

static void draw_row(cairo_t *cr, int i) {
    MprisPlayer *player = g_popup.players[i];
    RowLayout *row = &g_popup.rows[i];

    if (i == g_popup.hovered_index) {
        module_box_draw_rect(cr, OUTER_MARGIN, row->y, g_popup.content_width, row->h,
                              COLORS.surface1, (Color){ 0, 0, 0, 0 }, 0.0);
    }

    draw_thumbnail(cr, player, row);

    char glyph_buf[5];

    bool can_prev = mpris_player_can_go_previous(player);
    glyph_utf8(GLYPH_ICON_MEDIA_PREV, glyph_buf);
    module_box_draw_text_left(cr, control_prev_x(), row->controls_y, row->controls_h, glyph_buf, FONT.heading, false,
                               can_prev ? g_popup.accent : COLORS.overlay0);

    bool playing = mpris_player_is_playing(player);
    glyph_utf8(playing ? GLYPH_ICON_MEDIA_PAUSE : GLYPH_ICON_MEDIA_PLAY, glyph_buf);
    module_box_draw_text_left(cr, control_playpause_x(), row->controls_y, row->controls_h, glyph_buf, FONT.heading,
                               false, g_popup.accent);

    bool can_next = mpris_player_can_go_next(player);
    glyph_utf8(GLYPH_ICON_MEDIA_NEXT, glyph_buf);
    module_box_draw_text_left(cr, control_next_x(), row->controls_y, row->controls_h, glyph_buf, FONT.heading, false,
                               can_next ? g_popup.accent : COLORS.overlay0);

    module_box_draw_text_left(cr, control_track_x(), row->controls_y, row->controls_h, g_popup.track_text[i],
                               FONT.label, false, COLORS.text);

    if (row->has_time) {
        module_box_draw_text_left(cr, time_left_x(), row->time_y, row->time_h, row->time_left, FONT.sm, false,
                                   COLORS.subtext0);
        module_box_draw_text_left(cr, time_right_x(), row->time_y, row->time_h, row->time_right, FONT.sm, false,
                                   COLORS.subtext0);

        double bar_y = row->time_y + (row->time_h - TIMELINE_H) / 2.0;
        module_box_draw_rect(cr, timeline_x(), bar_y, TIMELINE_W, TIMELINE_H, COLORS.surface1, (Color){ 0, 0, 0, 0 },
                              0.0);
        double frac = row->progress_frac;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        double fill_w = TIMELINE_W * frac;
        if (fill_w > 0) {
            module_box_draw_rect(cr, timeline_x(), bar_y, fill_w, TIMELINE_H, g_popup.accent, (Color){ 0, 0, 0, 0 },
                                  0.0);
        }
    }
}

static void draw_popup(cairo_t *cr, void *user_data) {
    (void)user_data;
    module_box_draw_rect(cr, 0, 0, g_popup.popup_width, g_popup.popup_height,
                          COLORS.mantle, g_popup.accent, (double)BAR.module_border_width);
    for (int i = 0; i < g_popup.row_count; i++) {
        draw_row(cr, i);
    }
}

static void redraw(void) {
    if (g_popup.popup) {
        popup_surface_render_now(g_popup.popup, draw_popup, NULL);
    }
}

void media_popup_refresh_if_open(void) {
    if (!g_popup.open) {
        return;
    }
    compute_layout();
    redraw();
}

void media_popup_tick(void) {
    if (!g_popup.open) {
        return;
    }
    // MPRIS's Position property isn't push-notified via PropertiesChanged
    // per spec -- a player is expected to be polled for it while something
    // needs to show it moving, same as the reference MediaPopup.qml's own
    // `Timer { interval: 1000; running: root.visible }` re-triggering
    // positionChanged() while the popup is on screen. Called once a second
    // from main.c's existing clock timerfd handler rather than a new timer
    // of its own -- see CLAUDE.md's "Media (MPRIS)" section.
    for (int i = 0; i < g_popup.row_count; i++) {
        mpris_player_refresh_position(g_popup.players[i]);
    }
    compute_layout();
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

void media_popup_open(Output *output, Color accent, double box_x, double box_y, double box_w, double box_h) {
    (void)box_w; // left-anchored: only the box's left edge (box_x) matters, not its width
    media_popup_close(); // at most one media popup open at a time

    g_popup.accent = accent;
    g_popup.hovered_index = -1;
    g_popup.output = output;

    // Fetch every open player's current position once, up front, so the
    // very first frame already shows a real elapsed time rather than 0:00
    // until the first media_popup_tick() fires a second later.
    int count = mpris_player_count();
    if (count > MAX_MEDIA_ROWS) {
        count = MAX_MEDIA_ROWS;
    }
    for (int i = 0; i < count; i++) {
        mpris_player_refresh_position(mpris_player_get(i));
    }

    compute_layout();
    if (g_popup.row_count == 0) {
        return; // no tracked players at all -- nothing to show
    }

    // Same top_margin math (including the BAR.height correction) as every
    // other popup in this bar -- see sysmon/sysmon_info_popup.c for the
    // reasoning. Anchored TOP+LEFT since the media widget lives in the bar's
    // center region -- see media_popup.h's own doc comment.
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
    redraw();
}

void media_popup_close(void) {
    if (!g_popup.open) {
        return;
    }
    popup_surface_destroy(g_popup.popup);
    g_popup.popup = NULL;
    destroy_catchers();
    g_popup.open = false;
    g_popup.output = NULL;
    g_popup.row_count = 0;
    g_popup.hovered_index = -1;
}

bool media_popup_is_menu_surface(struct wl_surface *surface) {
    return g_popup.open && g_popup.popup && popup_surface_wl_surface(g_popup.popup) == surface;
}

bool media_popup_is_catcher_surface(struct wl_surface *surface) {
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
    for (int i = 0; i < g_popup.row_count; i++) {
        RowLayout *row = &g_popup.rows[i];
        if (y >= row->y && y < row->y + row->h) {
            return i;
        }
    }
    return -1;
}

void media_popup_handle_motion(double x, double y) {
    if (!g_popup.open) {
        return;
    }
    int idx = row_at(x, y);
    if (idx != g_popup.hovered_index) {
        g_popup.hovered_index = idx;
        redraw();
    }
}

void media_popup_handle_click(double x, double y) {
    if (!g_popup.open) {
        return;
    }
    int idx = row_at(x, y);
    if (idx < 0) {
        return;
    }

    RowLayout *row = &g_popup.rows[idx];
    // Only the controls sub-row (prev/playpause/next) is interactive --
    // landing on the thumbnail or the time/progress row is a no-op. No
    // click-to-seek on the progress bar in this pass: MPRIS's SetPosition()
    // needs the track's own mpris:trackid object path, which isn't cached
    // anywhere yet (only the fields this popup actually displays are), and
    // adding it purely to support seeking wasn't judged worth it here --
    // the progress bar is read-only, like the sysmon/clock popups' own
    // read-only content.
    if (y < row->controls_y || y >= row->controls_y + row->controls_h) {
        return;
    }

    MprisPlayer *player = g_popup.players[idx];

    if (x < content_x()) {
        return; // landed on the thumbnail column
    } else if (x < control_playpause_x()) {
        mpris_player_previous(player);
    } else if (x < control_next_x()) {
        mpris_player_toggle_playing(player);
    } else if (x < control_track_x()) {
        mpris_player_next(player);
    }
    // Landing on the track-text column is a no-op -- deliberately doesn't
    // close the popup either way, matching audio/device_popup.c's
    // device_popup_handle_click()'s identical "stay open" choice.
}
