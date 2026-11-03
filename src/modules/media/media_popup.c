#include "modules/media/media_popup.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "bar/popup_surface.h"
#include "config/glyphs.h"
#include "modules/media/media.h"
#include "render/meter_bar.h"
#include "render/module_box.h"

// Sized generously (this machine has 2 monitors), matching the same
// MAX_TRACKED_OUTPUTS convention used by every other popup in this project.
#define MAX_TRACKED_OUTPUTS 8

// Mirrors mpris/mpris_client.c's MAX_MPRIS_PLAYERS, but that constant isn't
// part of mpris_client.h's public surface -- redeclared locally at the same
// value, same reasoning audio/device_popup.c already gives for its own
// MAX_DEVICE_ROWS redeclaration.
#define MAX_MEDIA_ROWS 16

// Layout tuning, local to this popup (see CLAUDE.md "Conventions"). Header,
// pill and card styling match the CPU/GPU/memory dropdowns on purpose.
// The content width is fixed, so a track change (a longer title) never
// changes the popup's size -- titles truncate instead.
#define CONTENT_W 360.0
#define OUTER_MARGIN 12.0
#define SECTION_GAP 10.0
#define CARD_GAP 8.0
#define HEADER_H 22.0
#define PILL_H 16.0
#define PILL_PAD_X 7.0
#define CARD_PAD 10.0
#define TOP_LINE_H 14.0
#define TITLE_H 20.0
#define ARTIST_H 16.0
#define TIMELINE_GAP 6.0
#define TIMELINE_ROW_H 14.0
#define TIMELINE_METER_H 6.0
#define TIMELINE_SEGMENTS 36
#define CONTROLS_GAP 8.0
#define BUTTON_W 38.0
#define BUTTON_H 28.0
#define BUTTON_GAP 6.0
#define COL_GAP 10.0
#define TEXT_BUF_SIZE 600

// Prev/play-pause/next glyph size -- deliberately larger than the text so
// the controls read as buttons. Local tuning, not a shared FONT token.
#define CONTROL_ICON_PX 22

// Matches the reference MediaPopup.qml's own maxSaneDuration: some players
// (e.g. a browser tab playing a livestream) report a bogus sentinel length
// instead of correctly reporting "no known length" -- treat anything past
// 24h as "no duration" rather than showing a nonsense timeline.
#define MAX_SANE_DURATION_US (86400LL * 1000000LL)

enum { BUTTON_PREV, BUTTON_PLAYPAUSE, BUTTON_NEXT, BUTTON_COUNT };

typedef struct {
    MprisPlayer *player;
    double y, h;         // card rect, relative to the popup (x is always OUTER_MARGIN)
    bool has_artist;     // artist/album line shown (omitted if both are empty)
    double buttons_y;    // top of the control chips
} CardLayout;

typedef struct {
    bool open;
    Color accent;
    Output *output;
    PopupSurface *popup;
    int top_margin, left_margin; // kept from open time, for recreating the surface

    CardLayout cards[MAX_MEDIA_ROWS];
    int card_count;
    int hovered_card, hovered_button; // -1 = none
    double popup_width, popup_height;

    PopupSurface *catchers[MAX_TRACKED_OUTPUTS];
    struct wl_surface *catcher_surfaces[MAX_TRACKED_OUTPUTS];
    int catcher_count;
} MediaPopupState;

static MediaPopupState g_popup = { .hovered_card = -1, .hovered_button = -1 };

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

// Binary-search ellipsis truncation over UTF-8 codepoint boundaries -- see
// modules/media/media.c's own copy of this same algorithm (and
// audio/device_popup.c's original) for why it's duplicated locally rather
// than shared. Track titles/artists (unlike device_popup.c's ASCII-only
// device descriptions) routinely contain multi-byte UTF-8, so candidates are
// cut at codepoint offsets, not raw byte counts -- cutting mid-sequence
// produces invalid UTF-8 that pango_layout_set_text() (inside
// module_box_measure_text()) warns about and silently drops.
static void truncate_to_width(const char *text, double max_w, int font_px, bool bold, char *out, size_t out_size) {
    if (text_width(text, font_px, bold) <= max_w) {
        snprintf(out, out_size, "%s", text);
        return;
    }

    size_t len = strlen(text);
    size_t offsets[TEXT_BUF_SIZE];
    size_t n = 0;
    for (size_t i = 0; i < len && n < TEXT_BUF_SIZE - 1;) {
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
        char candidate[TEXT_BUF_SIZE + 4];
        snprintf(candidate, sizeof(candidate), "%.*s...", (int)offsets[mid], text);
        if (text_width(candidate, font_px, bold) <= max_w) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    snprintf(out, out_size, "%.*s...", (int)offsets[lo], text);
}

// "m:ss", or "h:mm:ss" from an hour up (long podcasts, videos) -- below an
// hour this matches MediaPopup.qml's own formatTime() exactly.
static void format_time(int64_t us, char *out, size_t out_size) {
    if (us < 0) {
        us = 0;
    }
    long long total = (long long)(us / 1000000);
    long long h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    if (h > 0) {
        snprintf(out, out_size, "%lld:%02lld:%02lld", h, m, s);
    } else {
        snprintf(out, out_size, "%lld:%02lld", m, s);
    }
}

// Status word and colour for a player's PlaybackStatus ("" -> stopped, the
// spec's state for a player that hasn't reported anything yet).
static const char *status_word(MprisPlayer *player, Color *color) {
    const char *status = mpris_player_status(player);
    if (strcmp(status, "Playing") == 0) {
        *color = COLORS.green;
        return "playing";
    }
    if (strcmp(status, "Paused") == 0) {
        *color = COLORS.yellow;
        return "paused";
    }
    *color = COLORS.overlay0;
    return "stopped";
}

static void artist_text(MprisPlayer *player, char *out, size_t out_size) {
    const char *artist = mpris_player_artist(player);
    const char *album = mpris_player_album(player);
    if (artist[0] && album[0]) {
        snprintf(out, out_size, "%s · %s", artist, album);
    } else {
        snprintf(out, out_size, "%s", artist[0] ? artist : album);
    }
}

static double card_x(void) { return OUTER_MARGIN; }
static double inner_x(void) { return OUTER_MARGIN + CARD_PAD; }
static double inner_w(void) { return CONTENT_W - CARD_PAD * 2.0; }
static double button_x(int button) { return inner_x() + button * (BUTTON_W + BUTTON_GAP); }

// Recomputes every card's geometry from mpris_client's live cache (already
// always-current, so there's nothing to snapshot) -- safe to re-run on every
// open, every live update and every 1s position tick. Only the height can
// change (player count, artist lines); the width is fixed.
static void compute_layout(void) {
    int count = mpris_player_count();
    if (count > MAX_MEDIA_ROWS) {
        count = MAX_MEDIA_ROWS; // documented limitation, matches sni_registry's own cap behavior
    }
    g_popup.card_count = count;

    double y = OUTER_MARGIN + HEADER_H + SECTION_GAP;
    for (int i = 0; i < count; i++) {
        CardLayout *card = &g_popup.cards[i];
        card->player = mpris_player_get(i);
        char artist[TEXT_BUF_SIZE];
        artist_text(card->player, artist, sizeof(artist));
        card->has_artist = artist[0] != '\0';

        double h = CARD_PAD + TOP_LINE_H + TITLE_H;
        if (card->has_artist) {
            h += ARTIST_H;
        }
        h += TIMELINE_GAP + TIMELINE_ROW_H + CONTROLS_GAP;
        card->buttons_y = y + h;
        h += BUTTON_H + CARD_PAD;

        card->y = y;
        card->h = h;
        y += h + (i < count - 1 ? CARD_GAP : 0.0);
    }
    g_popup.popup_width = CONTENT_W + OUTER_MARGIN * 2.0;
    g_popup.popup_height = y + OUTER_MARGIN;
}

// ---- Painting ----------------------------------------------------------------

static void paint_header(cairo_t *cr) {
    double x = OUTER_MARGIN, y = OUTER_MARGIN;
    char icon[5], summary[48];
    glyph_utf8(GLYPH_ICON_MEDIA_PLAY, icon);
    snprintf(summary, sizeof(summary), "  ·  %d player%s", g_popup.card_count, g_popup.card_count == 1 ? "" : "s");
    module_box_draw_text_left(cr, x, y, HEADER_H, icon, FONT.heading, false, g_popup.accent);
    double tx = x + text_width(icon, FONT.heading, false) + 8.0;
    module_box_draw_text_left(cr, tx, y, HEADER_H, "Media", FONT.md, true, COLORS.text);
    tx += text_width("Media", FONT.md, true);
    module_box_draw_text_left(cr, tx, y, HEADER_H, summary, FONT.sm, false, COLORS.subtext0);

    // Pill: the "liveliest" state across players -- playing beats paused
    // beats stopped.
    bool any_playing = false, any_paused = false;
    for (int i = 0; i < g_popup.card_count; i++) {
        const char *status = mpris_player_status(g_popup.cards[i].player);
        any_playing |= strcmp(status, "Playing") == 0;
        any_paused |= strcmp(status, "Paused") == 0;
    }
    const char *word = any_playing ? "playing" : any_paused ? "paused" : "stopped";
    Color fill = any_playing ? COLORS.green : any_paused ? COLORS.yellow : COLORS.overlay1;
    double pill_w = text_width(word, FONT.xs, true) + PILL_PAD_X * 2.0;
    double pill_x = x + CONTENT_W - pill_w;
    double pill_y = y + (HEADER_H - PILL_H) / 2.0;
    module_box_draw_rect(cr, pill_x, pill_y, pill_w, PILL_H, fill, COLORS.mantle, 0.0);
    module_box_draw_text_centered(cr, pill_x, pill_y, pill_w, PILL_H, word, FONT.xs, true, COLORS.mantle);
}

static void paint_timeline(cairo_t *cr, MprisPlayer *player, double y) {
    int64_t length_us = mpris_player_length_us(player);
    int64_t pos_us = mpris_player_position_us(player);
    bool has_length = length_us > 0 && length_us < MAX_SANE_DURATION_US;
    if (pos_us < 0) {
        pos_us = 0;
    }
    if (has_length && pos_us > length_us) {
        pos_us = length_us;
    }

    char pos[24], len[24], text[56];
    format_time(pos_us, pos, sizeof(pos));
    if (has_length) {
        format_time(length_us, len, sizeof(len));
        snprintf(text, sizeof(text), "%s / %s", pos, len);
    } else {
        // No (sane) length: the meter stays empty and only the elapsed time
        // shows -- a livestream, or a player that doesn't report one.
        snprintf(text, sizeof(text), "%s", pos);
    }
    // Reserve the widest the label can get, so the meter never shifts as
    // the seconds tick over (the font is monospace).
    double text_col_w = text_width("00:00:00 / 00:00:00", FONT.sm, false);
    double meter_w = inner_w() - text_col_w - COL_GAP;
    const Color gradient[3] = {g_popup.accent, g_popup.accent, COLORS.peach};
    double fraction = has_length ? (double)pos_us / (double)length_us : 0.0;
    meter_bar_draw(cr, inner_x(), y + (TIMELINE_ROW_H - TIMELINE_METER_H) / 2.0, meter_w, TIMELINE_METER_H, fraction,
                   TIMELINE_SEGMENTS, gradient, COLORS.surface0);
    draw_text_right(cr, inner_x() + inner_w(), y, TIMELINE_ROW_H, text, FONT.sm, false, COLORS.subtext0);
}

static void paint_button(cairo_t *cr, int card_index, int button, double y) {
    MprisPlayer *player = g_popup.cards[card_index].player;
    uint32_t glyph = button == BUTTON_PREV   ? GLYPH_ICON_MEDIA_PREV
                     : button == BUTTON_NEXT ? GLYPH_ICON_MEDIA_NEXT
                     : mpris_player_is_playing(player) ? GLYPH_ICON_MEDIA_PAUSE
                                                       : GLYPH_ICON_MEDIA_PLAY;
    bool enabled = button == BUTTON_PREV   ? mpris_player_can_go_previous(player)
                   : button == BUTTON_NEXT ? mpris_player_can_go_next(player)
                                           : true;
    bool hovered = enabled && card_index == g_popup.hovered_card && button == g_popup.hovered_button;
    double x = button_x(button);
    module_box_draw_rect(cr, x, y, BUTTON_W, BUTTON_H, hovered ? COLORS.surface1 : COLORS.surface0, COLORS.mantle,
                         0.0);
    char buf[5];
    glyph_utf8(glyph, buf);
    module_box_draw_text_centered(cr, x, y, BUTTON_W, BUTTON_H, buf, CONTROL_ICON_PX, false,
                                  enabled ? g_popup.accent : COLORS.overlay0);
}

static void paint_card(cairo_t *cr, int i) {
    CardLayout *card = &g_popup.cards[i];
    MprisPlayer *player = card->player;
    // The player the bar box shows gets the accent border, like the
    // calendar's current month.
    bool active = player == mpris_active_player();
    module_box_draw_rect(cr, card_x(), card->y, CONTENT_W, card->h, COLORS.base,
                         active ? g_popup.accent : COLORS.surface1, active ? 2.0 : 1.0);

    double y = card->y + CARD_PAD;
    char app[TEXT_BUF_SIZE], line[TEXT_BUF_SIZE];
    mpris_player_app_name(player, app, sizeof(app));
    for (char *p = app; *p; p++) {
        *p = (char)toupper((unsigned char)*p); // ASCII only; multi-byte bytes are left alone
    }
    Color status_color;
    const char *status = status_word(player, &status_color);
    double status_w = text_width(status, FONT.xs, true);
    truncate_to_width(app, inner_w() - status_w - COL_GAP, FONT.xs, true, line, sizeof(line));
    module_box_draw_text_left(cr, inner_x(), y, TOP_LINE_H, line, FONT.xs, true, COLORS.overlay0);
    draw_text_right(cr, inner_x() + inner_w(), y, TOP_LINE_H, status, FONT.xs, true, status_color);
    y += TOP_LINE_H;

    const char *title = mpris_player_title(player);
    if (!title[0]) {
        title = mpris_player_identity(player);
    }
    if (!title[0]) {
        title = "Unknown";
    }
    truncate_to_width(title, inner_w(), FONT.label, true, line, sizeof(line));
    module_box_draw_text_left(cr, inner_x(), y, TITLE_H, line, FONT.label, true, COLORS.text);
    y += TITLE_H;

    if (card->has_artist) {
        char artist[TEXT_BUF_SIZE];
        artist_text(player, artist, sizeof(artist));
        truncate_to_width(artist, inner_w(), FONT.md, false, line, sizeof(line));
        module_box_draw_text_left(cr, inner_x(), y, ARTIST_H, line, FONT.md, false, COLORS.subtext0);
        y += ARTIST_H;
    }

    paint_timeline(cr, player, y + TIMELINE_GAP);
    for (int b = 0; b < BUTTON_COUNT; b++) {
        paint_button(cr, i, b, card->buttons_y);
    }
}

static void draw_popup(cairo_t *cr, void *user_data) {
    (void)user_data;
    module_box_draw_rect(cr, 0, 0, g_popup.popup_width, g_popup.popup_height, COLORS.mantle, g_popup.accent,
                         (double)BAR.module_border_width);
    paint_header(cr);
    for (int i = 0; i < g_popup.card_count; i++) {
        paint_card(cr, i);
    }
}

static void redraw(void) {
    if (g_popup.popup) {
        popup_surface_render_now(g_popup.popup, draw_popup, NULL);
    }
}

static PopupSurface *create_menu_surface(void) {
    return popup_surface_create(g_popup.output->app, g_popup.output, (int)g_popup.popup_width,
                                (int)g_popup.popup_height, g_popup.top_margin, 0, 0, g_popup.left_margin,
                                ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
                                ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
}

// Re-lays out and repaints; recreates the menu surface if the height
// changed (a player appeared/disappeared, or gained/lost its artist line).
// Recreated rather than resized in place for the same reason as
// audio/mixer_popup.c: a set_size() on the existing layer surface made
// Hyprland unmap it on shrink. Closes the popup when no player is left.
static void relayout_and_redraw(void) {
    double old_h = g_popup.popup_height;
    compute_layout();
    if (g_popup.card_count == 0) {
        media_popup_close();
        return;
    }
    if (g_popup.hovered_card >= g_popup.card_count) {
        g_popup.hovered_card = -1;
        g_popup.hovered_button = -1;
    }
    if (g_popup.popup_height != old_h) {
        popup_surface_destroy(g_popup.popup);
        g_popup.popup = create_menu_surface();
        if (!g_popup.popup) {
            media_popup_close();
            return;
        }
    }
    redraw();
}

void media_popup_refresh_if_open(void) {
    if (!g_popup.open) {
        return;
    }
    relayout_and_redraw();
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
    for (int i = 0; i < g_popup.card_count; i++) {
        mpris_player_refresh_position(g_popup.cards[i].player);
    }
    relayout_and_redraw();
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

void media_popup_open(Output *output, Color accent, double box_x, double box_y, double box_w, double box_h) {
    (void)box_w; // left-anchored: only the box's left edge (box_x) matters, not its width
    media_popup_close(); // at most one media popup open at a time

    g_popup.accent = accent;
    g_popup.hovered_card = -1;
    g_popup.hovered_button = -1;
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
    if (g_popup.card_count == 0) {
        return; // no tracked players at all -- nothing to show
    }

    // Same top_margin math (including the BAR.height correction) as every
    // other popup in this bar -- see CLAUDE.md's "D-Bus / system tray"
    // for the reasoning. Anchored TOP+LEFT since the media widget lives in the bar's
    // center region -- see media_popup.h's own doc comment.
    g_popup.top_margin = (int)(box_y + box_h) - BAR.height;
    g_popup.left_margin = (int)box_x;
    g_popup.popup = create_menu_surface();
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
    g_popup.card_count = 0;
    g_popup.hovered_card = -1;
    g_popup.hovered_button = -1;
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

// Which control chip (card index + button) is under (x, y); false if none.
static bool button_at(double x, double y, int *card_out, int *button_out) {
    for (int i = 0; i < g_popup.card_count; i++) {
        CardLayout *card = &g_popup.cards[i];
        if (y < card->buttons_y || y >= card->buttons_y + BUTTON_H) {
            continue;
        }
        for (int b = 0; b < BUTTON_COUNT; b++) {
            if (x >= button_x(b) && x < button_x(b) + BUTTON_W) {
                *card_out = i;
                *button_out = b;
                return true;
            }
        }
    }
    return false;
}

void media_popup_handle_motion(double x, double y) {
    if (!g_popup.open) {
        return;
    }
    int card = -1, button = -1;
    button_at(x, y, &card, &button);
    if (card != g_popup.hovered_card || button != g_popup.hovered_button) {
        g_popup.hovered_card = card;
        g_popup.hovered_button = button;
        redraw();
    }
}

void media_popup_handle_click(double x, double y) {
    if (!g_popup.open) {
        return;
    }
    // Only the control chips are interactive -- the text and the timeline
    // are a no-op. No click-to-seek on the timeline: MPRIS's SetPosition()
    // needs the track's own mpris:trackid object path, which isn't cached
    // anywhere (only the fields this popup actually displays are), and
    // adding it purely to support seeking wasn't judged worth it here.
    int card = -1, button = -1;
    if (!button_at(x, y, &card, &button)) {
        return;
    }
    MprisPlayer *player = g_popup.cards[card].player;
    // prev/next already no-op when the player says it can't.
    if (button == BUTTON_PREV) {
        mpris_player_previous(player);
    } else if (button == BUTTON_PLAYPAUSE) {
        mpris_player_toggle_playing(player);
    } else {
        mpris_player_next(player);
    }
    // Deliberately doesn't close the popup, matching audio/device_popup.c's
    // device_popup_handle_press()'s identical "stay open" choice.
}

// ---- Test-control hooks (testing/test_control.c) ---------------------------------

int media_popup_test_card_count(void) { return g_popup.open ? g_popup.card_count : -1; }

bool media_popup_test_target(const char *app_name, const char *button, double *x, double *y) {
    if (!g_popup.open) {
        return false;
    }
    int b = strcmp(button, "prev") == 0 ? BUTTON_PREV : strcmp(button, "next") == 0 ? BUTTON_NEXT : BUTTON_PLAYPAUSE;
    for (int i = 0; i < g_popup.card_count; i++) {
        char name[TEXT_BUF_SIZE];
        mpris_player_app_name(g_popup.cards[i].player, name, sizeof(name));
        if (strcmp(name, app_name) != 0) {
            continue;
        }
        *x = button_x(b) + BUTTON_W / 2.0;
        *y = g_popup.cards[i].buttons_y + BUTTON_H / 2.0;
        return true;
    }
    return false;
}
