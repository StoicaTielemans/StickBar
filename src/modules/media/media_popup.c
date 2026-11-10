#include "modules/media/media_popup.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bar/popup_host.h"
#include "bar/popup_surface.h"
#include "bar/strip_surface.h"
#include "config/glyphs.h"
#include "modules/media/media.h"
#include "pipewire/pipewire_client.h"
#include "render/braille_graph.h"
#include "render/color.h"
#include "render/meter_bar.h"
#include "render/module_box.h"
#include "render/popup_chrome.h"
#include "render/popup_style.h"
#include "visualizer/visualizer.h"

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
#define TEXT_BUF_SIZE 600

// Spectrum graph, one per player card (visualizer/visualizer.c captures each
// player's own audio, stereo): drawn like the network dropdown's graph -- braille
// dots (render/braille_graph.c), the left channel rising toward a centre line,
// the right channel hanging below it, each half its own colour family, on a
// dark rounded panel whose left and right ends fade out. One dot column per
// frequency band (82 = 41 cells x 2) and 16 dot rows per half. Animated on its
// own subsurface (bar/strip_surface.c) from cached sprites -- see
// paint_graph() and media_popup_visualizer_frame().
#define VIS_ROW_GAP 10.0
#define VIS_CELLS 41          // braille cells across: 41 x 8px = 328px of the 340px panel
#define VIS_CELLS_H 4         // per half, like network_info_popup.c's GRAPH_CELLS_H
#define VIS_PAD_Y 6.0         // panel edge -> graph
#define VIS_CENTER_GAP 5.0    // between the halves; the centre line runs through it
                              // (network_info_popup.c's GRAPH_CENTER_GAP)
#define VIS_RADIUS 4.0
#define VIS_FADE_W 28.0       // soft left/right ends
#define VIS_HALF_H (VIS_CELLS_H * 14.0) // braille_graph_height(VIS_CELLS_H): 14px cells
#define VIS_ROW_H (2.0 * VIS_PAD_Y + 2.0 * VIS_HALF_H + VIS_CENTER_GAP) // whole panel
_Static_assert(SPECTRUM_BAR_COUNT == VIS_CELLS * 2, "one spectrum band per braille dot column");

// Prev/play-pause/next glyph size -- deliberately larger than the text so
// the controls read as buttons. Local tuning, not a shared FONT token.
#define CONTROL_ICON_PX 22

// Matches the reference MediaPopup.qml's own maxSaneDuration: some players
// (e.g. a browser tab playing a livestream) report a bogus sentinel length
// instead of correctly reporting "no known length" -- treat anything past
// 24h as "no duration" rather than showing a nonsense timeline.
#define MAX_SANE_DURATION_US (86400LL * 1000000LL)

enum { BUTTON_PREV, BUTTON_PLAYPAUSE, BUTTON_NEXT, BUTTON_COUNT };

// One frame of a spectrum graph in dot rows, per channel: how many dots each
// column has lit, and the row of its peak dot (0 = none). Compared whole with
// memcmp() -- a frame where no dot changed isn't committed at all.
typedef struct {
    int level[VISUALIZER_CHANNELS][SPECTRUM_BAR_COUNT];
    int peak[VISUALIZER_CHANNELS][SPECTRUM_BAR_COUNT];
} VisFrame;

// Per card (card index = visualizer slot, so only the first
// VISUALIZER_MAX_SOURCES cards get a graph): its strip subsurface, the frame it
// last drew, and which playback stream it captures.
typedef struct {
    StripSurface *strip;
    bool shown;
    VisFrame drawn;
    MprisPlayer *player; // whose stream `target` was resolved for
    uint32_t target;     // object.serial of its playback stream, or VISUALIZER_TARGET_MIX
} CardGraph;

typedef struct {
    MprisPlayer *player;
    double y, h;         // card rect, relative to the popup (x is always POPUP_OUTER_MARGIN)
    bool has_artist;     // artist/album line shown (omitted if both are empty)
    bool has_vis;        // spectrum graph shown (first VISUALIZER_MAX_SOURCES cards, if it runs)
    double vis_y;        // top of the graph panel, when has_vis
    double buttons_y;    // top of the control chips
} CardLayout;

typedef struct {
    PopupHost host; // the dropdown surface, its placement and click-catchers
    Color accent;

    // One per graph-carrying card; strips are children of `popup` and are
    // recreated with it.
    CardGraph graphs[VISUALIZER_MAX_SOURCES];

    CardLayout cards[MAX_MEDIA_ROWS];
    int card_count;
    int hovered_card, hovered_button; // -1 = none
    double popup_width, popup_height;

} MediaPopupState;

static MediaPopupState g_popup = { .hovered_card = -1, .hovered_button = -1 };

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

static double card_x(void) { return POPUP_OUTER_MARGIN; }
static double inner_x(void) { return POPUP_OUTER_MARGIN + CARD_PAD; }
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

    double y = POPUP_OUTER_MARGIN + POPUP_HEADER_H + POPUP_SECTION_GAP;
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
        // Every card gets the row whether or not its player is playing, so
        // play/pause never changes the popup's height (which would mean
        // recreating its surface, see relayout_and_redraw()).
        card->has_vis = visualizer_available() && i < VISUALIZER_MAX_SOURCES;
        if (card->has_vis) {
            card->vis_y = y + h + VIS_ROW_GAP;
            h += VIS_ROW_GAP + VIS_ROW_H;
        }
        h += TIMELINE_GAP + TIMELINE_ROW_H + CONTROLS_GAP;
        card->buttons_y = y + h;
        h += BUTTON_H + CARD_PAD;

        card->y = y;
        card->h = h;
        y += h + (i < count - 1 ? POPUP_CARD_GAP : 0.0);
    }
    g_popup.popup_width = CONTENT_W + POPUP_OUTER_MARGIN * 2.0;
    g_popup.popup_height = y + POPUP_OUTER_MARGIN;
}

// ---- Painting ----------------------------------------------------------------

static void paint_header(cairo_t *cr) {
    double x = POPUP_OUTER_MARGIN, y = POPUP_OUTER_MARGIN;
    char icon[5], summary[48];
    glyph_utf8(GLYPH_ICON_MEDIA_PLAY, icon);
    snprintf(summary, sizeof(summary), "  ·  %d player%s", g_popup.card_count, g_popup.card_count == 1 ? "" : "s");
    // Pill: the "liveliest" state across players -- playing beats paused
    // beats stopped.
    bool any_playing = false, any_paused = false;
    for (int i = 0; i < g_popup.card_count; i++) {
        const char *status = mpris_player_status(g_popup.cards[i].player);
        any_playing |= strcmp(status, "Playing") == 0;
        any_paused |= strcmp(status, "Paused") == 0;
    }
    PopupHeader header = {
        .icon = icon, .icon_color = g_popup.accent, .title = "Media", .summary = summary,
        .pill = any_playing ? "playing" : any_paused ? "paused" : "stopped",
        .pill_fill = any_playing ? COLORS.green : any_paused ? COLORS.yellow : COLORS.overlay1,
    };
    popup_draw_header(cr, x, y, CONTENT_W, &header);
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
    double text_col_w = module_box_text_width("00:00:00 / 00:00:00", FONT.sm, false);
    double meter_w = inner_w() - text_col_w - POPUP_COL_GAP;
    const Color gradient[3] = {g_popup.accent, g_popup.accent, COLORS.peach};
    double fraction = has_length ? (double)pos_us / (double)length_us : 0.0;
    meter_bar_draw(cr, inner_x(), y + (TIMELINE_ROW_H - TIMELINE_METER_H) / 2.0, meter_w, TIMELINE_METER_H, fraction,
                   TIMELINE_SEGMENTS, gradient, COLORS.surface0);
    module_box_draw_text_right(cr, inner_x() + inner_w(), y, TIMELINE_ROW_H, text, FONT.sm, false, COLORS.subtext0);
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

static double vis_panel_x(void) { return inner_x(); } // = the strip subsurface's left edge
static double vis_graph_x(void) { return floor((inner_w() - braille_graph_width(VIS_CELLS)) / 2.0); } // panel-local
static double vis_top_y(void) { return VIS_PAD_Y; }                                  // L half, panel-local
static double vis_bottom_y(void) { return VIS_PAD_Y + VIS_HALF_H + VIS_CENTER_GAP; } // R half

static void compute_frame(int slot, VisFrame *out) {
    int rows = braille_graph_rows(VIS_CELLS_H);
    for (int ch = 0; ch < VISUALIZER_CHANNELS; ch++) {
        const float *levels = visualizer_levels(slot, ch);
        const float *peaks = visualizer_peaks(slot, ch);
        for (int i = 0; i < SPECTRUM_BAR_COUNT; i++) {
            out->level[ch][i] = (int)lround(levels[i] * rows);
            int peak = (int)lround(peaks[i] * rows);
            // A peak dot only where it sits clear of the column; resting on
            // top it would just look like one more lit dot.
            out->peak[ch][i] = peak > out->level[ch][i] ? peak : 0;
        }
    }
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2.0, 0.0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0, M_PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2.0, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3.0 * M_PI / 2.0);
    cairo_close_path(cr);
}

// Rendered once per open (on first use), shared by every card's graph, freed
// in media_popup_close(). Three whole panels, all opaque and identical except
// for the dots:
//   g_vis_bg     the empty panel: the card's fill around its rounded corners,
//                the `mantle` panel, its `surface0` border, the centre line,
//                and the soft ends (`mantle` fading in over VIS_FADE_W at the
//                left and right, inside the border).
//   g_vis_lit    the same with every dot lit, coloured by height like the
//                network graph: L yellow -> peach -> red rising, R mauve ->
//                pink -> red hanging -- then the same fade on top.
//   g_vis_peak   the same with every dot lit in `text` (the peak dots).
// A frame is then a copy of g_vis_bg plus, per column, a copy of the lit
// rows' rectangle out of g_vis_lit (and the peak dot's out of g_vis_peak) --
// plain memcpy, see paint_graph(). The fade being baked into all three is
// what makes a straight copy correct at the faded ends.
static cairo_surface_t *g_vis_bg = NULL;
static cairo_surface_t *g_vis_lit = NULL;
static cairo_surface_t *g_vis_peak = NULL;

// The fade over a whole panel image, inside its 1px border.
static void paint_fade(cairo_t *cr, double w, double h) {
    for (int side = 0; side < 2; side++) {
        double x0 = side == 0 ? 1.0 : w - 1.0 - VIS_FADE_W;
        cairo_pattern_t *grad = cairo_pattern_create_linear(x0, 0, x0 + VIS_FADE_W, 0);
        double a0 = side == 0 ? 1.0 : 0.0, a1 = side == 0 ? 0.0 : 1.0;
        cairo_pattern_add_color_stop_rgba(grad, 0.0, COLORS.mantle.r, COLORS.mantle.g, COLORS.mantle.b, a0);
        cairo_pattern_add_color_stop_rgba(grad, 1.0, COLORS.mantle.r, COLORS.mantle.g, COLORS.mantle.b, a1);
        cairo_set_source(cr, grad);
        cairo_rectangle(cr, x0, 1.0, VIS_FADE_W, h - 2.0);
        cairo_fill(cr);
        cairo_pattern_destroy(grad);
    }
}

// One whole panel image: the empty panel, optionally with both halves' dots
// (`colors` = {L gradient, R gradient}) lit, then the fade.
static cairo_surface_t *render_panel(const Color (*colors)[3]) {
    double w = inner_w(), h = VIS_ROW_H;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)w, (int)h);
    cairo_t *cr = cairo_create(surf);
    color_set_source(cr, COLORS.base);
    cairo_paint(cr);
    rounded_rect(cr, 0.5, 0.5, w - 1.0, h - 1.0, VIS_RADIUS);
    color_set_source(cr, COLORS.mantle);
    cairo_fill_preserve(cr);
    color_set_source(cr, COLORS.surface0);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    color_set_source(cr, COLORS.surface1);
    cairo_rectangle(cr, vis_graph_x(), floor(VIS_PAD_Y + VIS_HALF_H + VIS_CENTER_GAP / 2.0),
                    braille_graph_width(VIS_CELLS), 1.0);
    cairo_fill(cr);
    if (colors) {
        double half_y[VISUALIZER_CHANNELS] = {vis_top_y(), vis_bottom_y()};
        for (int ch = 0; ch < VISUALIZER_CHANNELS; ch++) {
            cairo_surface_t *dots = braille_graph_render_lit(VIS_CELLS, VIS_CELLS_H, ch == VISUALIZER_LEFT,
                                                             colors[ch]);
            cairo_set_source_surface(cr, dots, vis_graph_x(), half_y[ch]);
            cairo_paint(cr);
            cairo_surface_destroy(dots);
        }
    }
    paint_fade(cr, w, h);
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    return surf;
}

static void build_vis_cache(void) {
    const Color lit[VISUALIZER_CHANNELS][3] = {
        {COLORS.yellow, COLORS.peach, COLORS.red}, // L, rising
        {COLORS.mauve, COLORS.pink, COLORS.red},   // R, hanging
    };
    const Color peak[VISUALIZER_CHANNELS][3] = {
        {COLORS.text, COLORS.text, COLORS.text},
        {COLORS.text, COLORS.text, COLORS.text},
    };
    g_vis_bg = render_panel(NULL);
    g_vis_lit = render_panel(lit);
    g_vis_peak = render_panel(peak);
}

static void free_vis_cache(void) {
    cairo_surface_t **all[] = {&g_vis_bg, &g_vis_lit, &g_vis_peak};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (*all[i]) {
            cairo_surface_destroy(*all[i]);
            *all[i] = NULL;
        }
    }
}

// Copies panel-local rectangle (x, y, w, h) from `src` (a whole-panel image)
// into the panel at `dst` (its top-left pixel; `dst_stride` bytes per row).
static void copy_rect(uint8_t *dst, int dst_stride, cairo_surface_t *src, int x, int y, int w, int h) {
    const uint8_t *s = cairo_image_surface_get_data(src);
    int s_stride = cairo_image_surface_get_stride(src);
    for (int row = 0; row < h; row++) {
        memcpy(dst + (size_t)(y + row) * dst_stride + (size_t)x * 4, s + (size_t)(y + row) * s_stride + (size_t)x * 4,
               (size_t)w * 4);
    }
}

// Paints one whole graph panel at the current origin of `cr` (panel-local:
// (0, 0) is its top-left) -- straight into the target's pixels, see the
// cache comment above. Used for the subsurfaces' frames, and also inside the
// popup's own full paint (translated): there it's hidden under the
// subsurface, but it's what makes `snapshot` PNGs (which only re-run the
// popup's draw callback) show it. Both targets are ARGB32 image surfaces.
static void paint_graph(cairo_t *cr, const VisFrame *f) {
    if (!g_vis_bg) {
        build_vis_cache();
    }
    cairo_surface_t *target = cairo_get_target(cr);
    if (cairo_surface_get_type(target) != CAIRO_SURFACE_TYPE_IMAGE ||
        cairo_image_surface_get_format(target) != CAIRO_FORMAT_ARGB32) {
        return;
    }
    double ox = 0.0, oy = 0.0;
    cairo_user_to_device(cr, &ox, &oy); // where the panel's (0, 0) lands
    cairo_surface_flush(target);
    int stride = cairo_image_surface_get_stride(target);
    uint8_t *dst = cairo_image_surface_get_data(target) + (size_t)lround(oy) * stride + (size_t)lround(ox) * 4;

    int w = (int)inner_w(), h = (int)VIS_ROW_H;
    copy_rect(dst, stride, g_vis_bg, 0, 0, w, h);
    int gx = (int)vis_graph_x();
    int half_y[VISUALIZER_CHANNELS] = {(int)vis_top_y(), (int)vis_bottom_y()};
    for (int ch = 0; ch < VISUALIZER_CHANNELS; ch++) {
        bool grow_up = ch == VISUALIZER_LEFT;
        for (int col = 0; col < SPECTRUM_BAR_COUNT; col++) {
            int rx, ry, rw, rh;
            if (f->level[ch][col] > 0) {
                braille_graph_column_rect(VIS_CELLS_H, col, 0, f->level[ch][col], grow_up, &rx, &ry, &rw, &rh);
                copy_rect(dst, stride, g_vis_lit, gx + rx, half_y[ch] + ry, rw, rh);
            }
            if (f->peak[ch][col] > 0) {
                int p = f->peak[ch][col];
                braille_graph_column_rect(VIS_CELLS_H, col, p - 1, p, grow_up, &rx, &ry, &rw, &rh);
                copy_rect(dst, stride, g_vis_peak, gx + rx, half_y[ch] + ry, rw, rh);
            }
        }
    }
    cairo_surface_mark_dirty(target);
}

static void paint_strip(cairo_t *cr, int w, int h, void *user_data) {
    (void)w;
    (void)h;
    paint_graph(cr, user_data);
}

static void paint_visualizer(cairo_t *cr, int slot, double y) {
    VisFrame f;
    compute_frame(slot, &f);
    cairo_save(cr);
    cairo_translate(cr, vis_panel_x(), y);
    paint_graph(cr, &f);
    cairo_restore(cr);
}

static void paint_card(cairo_t *cr, int i) {
    CardLayout *card = &g_popup.cards[i];
    MprisPlayer *player = card->player;
    // The player the bar box shows gets the accent border, like the
    // calendar's current month.
    bool active = player == mpris_active_player();
    popup_draw_card(cr, card_x(), card->y, CONTENT_W, card->h, active, false, g_popup.accent);

    double y = card->y + CARD_PAD;
    char app[TEXT_BUF_SIZE], line[TEXT_BUF_SIZE];
    mpris_player_app_name(player, app, sizeof(app));
    for (char *p = app; *p; p++) {
        *p = (char)toupper((unsigned char)*p); // ASCII only; multi-byte bytes are left alone
    }
    Color status_color;
    const char *status = status_word(player, &status_color);
    double status_w = module_box_text_width(status, FONT.xs, true);
    module_box_truncate_to_width(app, inner_w() - status_w - POPUP_COL_GAP, FONT.xs, true, line, sizeof(line));
    module_box_draw_text_left(cr, inner_x(), y, TOP_LINE_H, line, FONT.xs, true, COLORS.overlay0);
    module_box_draw_text_right(cr, inner_x() + inner_w(), y, TOP_LINE_H, status, FONT.xs, true, status_color);
    y += TOP_LINE_H;

    const char *title = media_display_title(player);
    module_box_truncate_to_width(title, inner_w(), FONT.label, true, line, sizeof(line));
    module_box_draw_text_left(cr, inner_x(), y, TITLE_H, line, FONT.label, true, COLORS.text);
    y += TITLE_H;

    if (card->has_artist) {
        char artist[TEXT_BUF_SIZE];
        artist_text(player, artist, sizeof(artist));
        module_box_truncate_to_width(artist, inner_w(), FONT.md, false, line, sizeof(line));
        module_box_draw_text_left(cr, inner_x(), y, ARTIST_H, line, FONT.md, false, COLORS.subtext0);
        y += ARTIST_H;
    }

    if (card->has_vis) {
        paint_visualizer(cr, i, card->vis_y);
        y = card->vis_y + VIS_ROW_H;
    }

    paint_timeline(cr, player, y + TIMELINE_GAP);
    for (int b = 0; b < BUTTON_COUNT; b++) {
        paint_button(cr, i, b, card->buttons_y);
    }
}

static void draw_popup(cairo_t *cr, void *user_data) {
    (void)user_data;
    popup_draw_frame(cr, g_popup.popup_width, g_popup.popup_height, g_popup.accent);
    paint_header(cr);
    for (int i = 0; i < g_popup.card_count; i++) {
        paint_card(cr, i);
    }
}

static void redraw(void) {
    if (g_popup.host.popup) {
        popup_surface_render_now(g_popup.host.popup, draw_popup, NULL);
    }
}

static void update_graphs(void); // below, with the rest of the visualizer code
static void destroy_graphs(void);

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
        destroy_graphs(); // the strips are children of the surface about to go away
        if (!popup_host_resize(&g_popup.host, (int)g_popup.popup_width, (int)g_popup.popup_height)) {
            media_popup_close();
            return;
        }
    }
    update_graphs();
    redraw();
}

static void destroy_graphs(void) {
    for (int i = 0; i < VISUALIZER_MAX_SOURCES; i++) {
        strip_surface_destroy(g_popup.graphs[i].strip);
        g_popup.graphs[i].strip = NULL;
        g_popup.graphs[i].shown = false;
    }
}

// Which playback stream card `i`'s graph should capture: the player's own
// (matched by process, see pipewire_find_playback_for_pid()), or the whole
// output mix if it can't be found -- better a graph of everything than none.
static void resolve_target(int i) {
    CardGraph *g = &g_popup.graphs[i];
    g->player = g_popup.cards[i].player;
    uint32_t serial = 0;
    g->target = pipewire_find_playback_for_pid(mpris_player_pid(g->player), &serial) ? serial
                                                                                    : VISUALIZER_TARGET_MIX;
}

// Places each graph-carrying card's strip subsurface over its graph row
// (creating it on the current popup surface if needed) and draws it; hides
// the rest. Called after every relayout -- rows move when players come and go
// -- before the popup's own commit, which is what applies new positions. A
// card whose player changed gets its stream re-resolved (it isn't re-resolved
// otherwise; see media_popup_streams_changed()).
static void update_graphs(void) {
    struct wl_surface *parent = g_popup.host.popup ? popup_surface_wl_surface(g_popup.host.popup) : NULL;
    for (int i = 0; i < VISUALIZER_MAX_SOURCES; i++) {
        CardGraph *g = &g_popup.graphs[i];
        if (g->strip && strip_surface_parent(g->strip) != parent) {
            strip_surface_destroy(g->strip); // popup surface was recreated
            g->strip = NULL;
            g->shown = false;
        }
        const CardLayout *card = i < g_popup.card_count ? &g_popup.cards[i] : NULL;
        if (!card || !card->has_vis || !parent) {
            if (g->strip) {
                strip_surface_hide(g->strip);
            }
            g->shown = false;
            g->player = NULL;
            continue;
        }
        if (g->player != card->player) {
            resolve_target(i);
        }
        if (!g->strip) {
            g->strip = strip_surface_create(g_popup.host.output->app, parent);
            if (!g->strip) {
                continue;
            }
        }
        strip_surface_place(g->strip, (int)vis_panel_x(), (int)card->vis_y, (int)inner_w(), (int)VIS_ROW_H);
        compute_frame(i, &g->drawn);
        strip_surface_draw(g->strip, paint_strip, &g->drawn);
        g->shown = true;
    }
}

// Test-only: tests/test_visualizer.py sets STICKBAR_TEST_VISUALIZER_ONLY to
// its fake players' name prefix, so its bar never captures (or reads) a real
// player the user has playing at the time.
static bool capture_allowed(MprisPlayer *player) {
    static const char *only = NULL;
    static bool read_env = false;
    if (!read_env) {
        only = getenv("STICKBAR_TEST_VISUALIZER_ONLY");
        read_env = true;
    }
    if (!only || !only[0]) {
        return true;
    }
    char app[64];
    mpris_player_app_name(player, app, sizeof(app));
    return strncmp(app, only, strlen(only)) == 0;
}

void media_popup_sync_visualizer(void) {
    for (int i = 0; i < VISUALIZER_MAX_SOURCES; i++) {
        CardGraph *g = &g_popup.graphs[i];
        bool active = g_popup.host.open && g->shown && g->player && mpris_player_is_playing(g->player) &&
                      capture_allowed(g->player);
        visualizer_set_source(i, g->target, active);
    }
}

void media_popup_streams_changed(void) {
    if (!g_popup.host.open) {
        return;
    }
    // Streams come and go (a browser opens a new one per track, Spotify keeps
    // an idle one around), so re-match every shown card. The next
    // media_popup_sync_visualizer() moves a slot whose target changed.
    for (int i = 0; i < VISUALIZER_MAX_SOURCES; i++) {
        if (g_popup.graphs[i].shown) {
            resolve_target(i);
        }
    }
}

void media_popup_visualizer_frame(uint32_t changed_mask) {
    if (!g_popup.host.open) {
        return;
    }
    for (int i = 0; i < VISUALIZER_MAX_SOURCES; i++) {
        CardGraph *g = &g_popup.graphs[i];
        if (!(changed_mask & (1u << i)) || !g->shown) {
            continue;
        }
        VisFrame f;
        compute_frame(i, &f);
        if (memcmp(&f, &g->drawn, sizeof(f)) == 0) {
            continue; // no dot changed -- no commit
        }
        g->drawn = f;
        // Only this graph's own strip -- the popup's text, timeline and
        // buttons aren't touched.
        strip_surface_draw(g->strip, paint_strip, &g->drawn);
    }
}

void media_popup_visualizer_status(uint32_t *shown_mask, uint32_t *ready_mask) {
    *shown_mask = 0;
    *ready_mask = 0;
    if (!g_popup.host.open) {
        return;
    }
    for (int i = 0; i < VISUALIZER_MAX_SOURCES; i++) {
        CardGraph *g = &g_popup.graphs[i];
        if (g->shown) {
            *shown_mask |= 1u << i;
            if (strip_surface_ready(g->strip)) {
                *ready_mask |= 1u << i;
            }
        }
    }
}

void media_popup_refresh_if_open(void) {
    if (!g_popup.host.open) {
        return;
    }
    relayout_and_redraw();
}

void media_popup_tick(void) {
    if (!g_popup.host.open) {
        return;
    }
    // MPRIS's Position property isn't push-notified via PropertiesChanged
    // per spec -- a player is expected to be polled for it while something
    // needs to show it moving, same as the reference MediaPopup.qml's own
    // `Timer { interval: 1000; running: root.visible }` re-triggering
    // positionChanged() while the popup is on screen. Called once a second
    // from main.c's existing clock timerfd handler rather than a new timer
    // of its own -- see docs/modules/media.md.
    for (int i = 0; i < g_popup.card_count; i++) {
        mpris_player_refresh_position(g_popup.cards[i].player);
    }
    relayout_and_redraw();
}

void media_popup_open(Output *output, Color accent, double box_x, double box_y, double box_w, double box_h) {
    media_popup_close(); // at most one media popup open at a time

    g_popup.accent = accent;
    g_popup.hovered_card = -1;
    g_popup.hovered_button = -1;
    g_popup.host.output = output;

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

    // Under its box, opening rightward from the box's left edge (bar/popup_host.c).
    if (!popup_host_open(&g_popup.host, output, POPUP_ANCHOR_LEFT, box_x, box_y, box_w, box_h, (int)g_popup.popup_width,
                         (int)g_popup.popup_height)) {
        return;
    }
    update_graphs();
    redraw();
}

void media_popup_close(void) {
    if (!g_popup.host.open) {
        return;
    }
    destroy_graphs(); // before their parent
    for (int i = 0; i < VISUALIZER_MAX_SOURCES; i++) {
        g_popup.graphs[i].player = NULL;
    }
    free_vis_cache();
    popup_host_close(&g_popup.host);
    g_popup.card_count = 0;
    g_popup.hovered_card = -1;
    g_popup.hovered_button = -1;
}

bool media_popup_is_menu_surface(struct wl_surface *surface) {
    return popup_host_is_menu_surface(&g_popup.host, surface);
}

bool media_popup_is_catcher_surface(struct wl_surface *surface) {
    return popup_host_is_catcher_surface(&g_popup.host, surface);
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
    if (!g_popup.host.open) {
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
    if (!g_popup.host.open) {
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

int media_popup_test_card_count(void) { return g_popup.host.open ? g_popup.card_count : -1; }

bool media_popup_test_graph(int card, MediaPopupTestGraph *out) {
    if (!g_popup.host.open || card < 0 || card >= g_popup.card_count || !g_popup.cards[card].has_vis) {
        return false;
    }
    mpris_player_app_name(g_popup.cards[card].player, out->app, sizeof(out->app));
    out->slot = card;
    out->target = g_popup.graphs[card].target;
    out->panel_x = vis_panel_x();
    out->panel_y = g_popup.cards[card].vis_y;
    out->panel_w = inner_w();
    out->panel_h = VIS_ROW_H;
    out->graph_x = vis_panel_x() + vis_graph_x();
    out->top_y = g_popup.cards[card].vis_y + vis_top_y();
    out->bottom_y = g_popup.cards[card].vis_y + vis_bottom_y();
    out->half_h = VIS_HALF_H;
    out->rows = braille_graph_rows(VIS_CELLS_H);
    return true;
}

bool media_popup_test_target(const char *app_name, const char *button, double *x, double *y) {
    if (!g_popup.host.open) {
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
