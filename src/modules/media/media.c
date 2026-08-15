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

// Matches MediaWidget.qml's trackText(): "artist - title", falling back to
// the player's own identity (then "Unknown") when no title is reported.
void media_build_track_text(MprisPlayer *player, char *out, size_t out_size) {
    const char *artist = mpris_player_artist(player);
    const char *title = mpris_player_title(player);
    if (!title[0]) {
        title = mpris_player_identity(player);
    }
    if (!title[0]) {
        title = "Unknown";
    }
    if (artist[0]) {
        snprintf(out, out_size, "%s - %s", artist, title);
    } else {
        snprintf(out, out_size, "%s", title);
    }
}

// Binary-search ellipsis truncation over UTF-8 codepoint boundaries,
// mirroring audio/device_popup.c's own truncate_to_width() -- duplicated
// rather than shared since each caller's font/weight/max-width differ and
// this project's convention (see every module's own MAX_TRACKED_OUTPUTS
// block) already favors small local duplication over a shared helper for
// widget-specific tuning like this. Track titles/artists routinely contain
// multi-byte UTF-8, so candidates are cut at codepoint offsets, not raw byte
// counts -- cutting mid-sequence produces invalid UTF-8 that
// pango_layout_set_text() (inside module_box_measure_text()) warns about and
// silently drops.
static void truncate_track_text(const char *text, double max_w, int font_px, bool bold, char *out,
                                 size_t out_size) {
    double w, h;
    module_box_measure_text(text, font_px, bold, &w, &h);
    if (w <= max_w) {
        snprintf(out, out_size, "%s", text);
        return;
    }

    size_t len = strlen(text);
    size_t offsets[512];
    size_t n = 0;
    for (size_t i = 0; i < len && n < sizeof(offsets) / sizeof(offsets[0]) - 1;) {
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
        char candidate[512];
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

void media_render(cairo_t *cr, Output *output, double bar_width, double box_top, double box_height) {
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

    char track_text[512];
    media_build_track_text(player, track_text, sizeof(track_text));
    char truncated[512];
    truncate_track_text(track_text, TRACK_TEXT_MAX_W, FONT.label, true, truncated, sizeof(truncated));

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
