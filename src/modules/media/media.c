#include "modules/media/media.h"

#include <linux/input-event-codes.h> // BTN_LEFT, BTN_RIGHT
#include <stdio.h>
#include <string.h>

#include "config/config.h"
#include "config/glyphs.h"
#include "modules/media/media_popup.h"
#include "mpris/mpris_client.h"
#include "render/module_box.h"

// How many different Output pointers we keep the box's hit-rect for -- sized
// generously (this machine has 2 monitors), matching every other per-output
// hit-rect table in this project.
#define MAX_TRACKED_OUTPUTS 8

// Matches the reference MediaWidget.qml's own `width: Math.min(implicitWidth, 300)`
// cap on the track-text label.
#define TRACK_TEXT_MAX_W 300.0

typedef struct {
    Output *output; // NULL if this slot is unused
    double x, y, w, h;
} OutputBoxRect;

static OutputBoxRect g_rects[MAX_TRACKED_OUTPUTS];

static OutputBoxRect *rect_for_output(Output *output) {
    OutputBoxRect *free_slot = NULL;
    for (int i = 0; i < MAX_TRACKED_OUTPUTS; i++) {
        if (g_rects[i].output == output) {
            return &g_rects[i];
        }
        if (!free_slot && g_rects[i].output == NULL) {
            free_slot = &g_rects[i];
        }
    }
    if (free_slot) {
        free_slot->output = output;
    }
    return free_slot;
}

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
    OutputBoxRect *rect = rect_for_output(output);

    if (!player) {
        // No active player -- matches the reference's own
        // `visible: player !== null` / zero implicitWidth: nothing is drawn,
        // and the hit-rect is cleared so a stale click can't land on a box
        // that's no longer shown.
        if (rect) {
            rect->w = 0;
            rect->h = 0;
        }
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
    double max_text_w = max_box_w - 12.0 - prefix_w;
    if (max_text_w > TRACK_TEXT_MAX_W) {
        max_text_w = TRACK_TEXT_MAX_W;
    }
    if (max_text_w < ellipsis_w) {
        // Not even "..." fits -- collapse exactly like the no-player case.
        if (rect) {
            rect->w = 0;
            rect->h = 0;
        }
        return;
    }

    char track_text[512];
    build_track_text(player, track_text, sizeof(track_text));
    char truncated[512];
    module_box_truncate_to_width(track_text, max_text_w, FONT.label, true, truncated, sizeof(truncated));

    char label[600];
    snprintf(label, sizeof(label), "%s  %s", icon_buf, truncated);

    double text_w, text_h;
    module_box_measure_text(label, FONT.label, true, &text_w, &text_h);
    (void)text_h;

    // Matches this project's other module boxes' `implicitWidth: label + 12`
    // convention.
    double box_w = text_w + 12.0;
    double box_x = (bar_width - box_w) / 2.0;

    module_box_draw_rect(cr, box_x, box_top, box_w, box_height,
                          COLORS.mantle, COLORS.yellow, (double)BAR.module_border_width);
    module_box_draw_text_centered(cr, box_x, box_top, box_w, box_height, label, FONT.label, true, COLORS.yellow);

    if (rect) {
        rect->x = box_x;
        rect->y = box_top;
        rect->w = box_w;
        rect->h = box_height;
    }
}

bool media_last_rect(Output *output, double *x, double *y, double *w, double *h) {
    for (int i = 0; i < MAX_TRACKED_OUTPUTS; i++) {
        if (g_rects[i].output == output && g_rects[i].w > 0) {
            *x = g_rects[i].x;
            *y = g_rects[i].y;
            *w = g_rects[i].w;
            *h = g_rects[i].h;
            return true;
        }
    }
    return false;
}

void media_handle_click(Output *output, double x, double y, uint32_t button) {
    OutputBoxRect *rect = NULL;
    for (int i = 0; i < MAX_TRACKED_OUTPUTS; i++) {
        if (g_rects[i].output == output) {
            rect = &g_rects[i];
            break;
        }
    }
    if (!rect || rect->w <= 0 || x < rect->x || x >= rect->x + rect->w || y < rect->y || y >= rect->y + rect->h) {
        return;
    }

    if (button == BTN_LEFT) {
        mpris_player_toggle_playing(mpris_active_player());
    } else if (button == BTN_RIGHT) {
        media_popup_open(output, COLORS.yellow, rect->x, rect->y, rect->w, rect->h);
    }
}
