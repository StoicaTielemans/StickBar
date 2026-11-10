#include "modules/media/media.h"

#include <linux/input-event-codes.h> // BTN_LEFT, BTN_RIGHT
#include <stdio.h>

#include "config/config.h"
#include "config/glyphs.h"
#include "modules/media/media_popup.h"
#include "mpris/mpris_client.h"
#include "render/box_rects.h"
#include "render/module_box.h"

// Matches the reference MediaWidget.qml's own `width: Math.min(implicitWidth, 300)`
// cap on the track-text label.
#define TRACK_TEXT_MAX_W 300.0

// Where this box was last drawn, per output (render/box_rects.h).
static BoxRectTable g_rects;

const char *media_display_title(MprisPlayer *player) {
    const char *title = mpris_player_title(player);
    if (!title[0]) {
        title = mpris_player_identity(player);
    }
    return title[0] ? title : "Unknown";
}

// Matches MediaWidget.qml's trackText(): "artist - title", the title as
// media_display_title() gives it.
static void build_track_text(MprisPlayer *player, char *out, size_t out_size) {
    const char *artist = mpris_player_artist(player);
    const char *title = media_display_title(player);
    if (artist[0]) {
        snprintf(out, out_size, "%s - %s", artist, title);
    } else {
        snprintf(out, out_size, "%s", title);
    }
}

void media_render(cairo_t *cr, Output *output, double bar_width, double left_limit, double right_limit,
                  double box_top, double box_height) {
    MprisPlayer *player = mpris_active_player();

    if (!player) {
        // No active player -- matches the reference's own
        // `visible: player !== null` / zero implicitWidth: nothing is drawn,
        // and the hit-rect is cleared so a stale click can't land on a box
        // that's no longer shown.
        box_rect_clear(&g_rects, output);
        return;
    }

    char icon_buf[5];
    glyph_utf8(mpris_player_is_playing(player) ? GLYPH_ICON_MEDIA_PAUSE : GLYPH_ICON_MEDIA_PLAY, icon_buf);

    // Width actually free around the bar's center: the box stays centered, so
    // it can only be twice as wide as the *nearer* of the two side regions
    // allows. Without this, a long title on a narrow output (or a busy bar)
    // drew straight over the GPU/mem/privacy or mic/EQ boxes.
    double center_x = bar_width / 2.0;
    double free_half = center_x - left_limit;
    if (right_limit - center_x < free_half) {
        free_half = right_limit - center_x;
    }
    double max_box_w = 2.0 * free_half;

    // Text budget = box budget minus the 12px box padding and the
    // "<icon>  " prefix, still capped at the reference's own 300px.
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "%s  ", icon_buf);
    double prefix_w, prefix_h, ellipsis_w;
    module_box_measure_text(prefix, FONT.label, true, &prefix_w, &prefix_h);
    module_box_measure_text("...", FONT.label, true, &ellipsis_w, &prefix_h);
    double max_text_w = max_box_w - BAR.module_padding - prefix_w;
    if (max_text_w > TRACK_TEXT_MAX_W) {
        max_text_w = TRACK_TEXT_MAX_W;
    }
    if (max_text_w < ellipsis_w) {
        // Not even "..." fits -- collapse exactly like the no-player case.
        box_rect_clear(&g_rects, output);
        return;
    }

    char track_text[512];
    build_track_text(player, track_text, sizeof(track_text));
    char truncated[512];
    module_box_truncate_to_width(track_text, max_text_w, FONT.label, true, truncated, sizeof(truncated));

    char label[600];
    snprintf(label, sizeof(label), "%s  %s", icon_buf, truncated);

    // Centered: the standard box, laid out from the left edge that centers it.
    double box_w = module_box_text_width(label, FONT.label, true) + BAR.module_padding;
    module_box_label(cr, &g_rects, output, MODULE_BOX_FROM_LEFT, (bar_width - box_w) / 2.0, box_top, box_height, label,
                     COLORS.yellow);
}

bool media_last_rect(Output *output, double *x, double *y, double *w, double *h) {
    BoxRect r;
    if (!box_rect_get(&g_rects, output, &r)) {
        return false;
    }
    *x = r.x;
    *y = r.y;
    *w = r.w;
    *h = r.h;
    return true;
}

void media_handle_click(Output *output, double x, double y, uint32_t button) {
    const BoxRect *rect = box_rect_hit(&g_rects, output, x, y);
    if (!rect) {
        return;
    }

    if (button == BTN_LEFT) {
        mpris_player_toggle_playing(mpris_active_player());
    } else if (button == BTN_RIGHT) {
        media_popup_open(output, COLORS.yellow, rect->x, rect->y, rect->w, rect->h);
    }
}
