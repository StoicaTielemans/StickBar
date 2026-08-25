#include "modules/privacy/privacy.h"

#include <linux/input-event-codes.h> // BTN_RIGHT
#include <stdio.h>
#include <string.h>

#include "config/config.h"
#include "config/glyphs.h"
#include "modules/privacy/privacy_info_popup.h"
#include "pipewire/pipewire_client.h"
#include "render/module_box.h"

// How many different Output pointers we keep the box's hit-rect for -- sized
// generously (this machine has 2 monitors), matching the convention already
// used by every other per-output hit-rect table in this project.
#define MAX_TRACKED_OUTPUTS 8

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

double privacy_render(cairo_t *cr, Output *output, double x, double box_top, double box_height) {
    bool mic = pipewire_client_mic_active();
    bool webcam = pipewire_client_webcam_active();
    bool screenshare = pipewire_client_screenshare_active();

    OutputBoxRect *rect = rect_for_output(output);

    if (!mic && !webcam && !screenshare) {
        if (rect) {
            rect->w = 0;
            rect->h = 0;
        }
        return x; // unadvanced -- next left-region module starts exactly here
    }

    // One icon per active category, space-joined -- no percent/text like
    // cpu/gpu/mem's own boxes, just the glyph(s).
    char label[32] = "";
    char icon_buf[5];
    bool first = true;
    if (mic) {
        glyph_utf8(GLYPH_ICON_MIC, icon_buf);
        strcat(label, icon_buf);
        first = false;
    }
    if (webcam) {
        if (!first) strcat(label, " ");
        glyph_utf8(GLYPH_ICON_WEBCAM, icon_buf);
        strcat(label, icon_buf);
        first = false;
    }
    if (screenshare) {
        if (!first) strcat(label, " ");
        glyph_utf8(GLYPH_ICON_SCREENSHARE, icon_buf);
        strcat(label, icon_buf);
    }

    double text_w, text_h;
    module_box_measure_text(label, FONT.label, true, &text_w, &text_h);
    (void)text_h;

    // Matches this project's other module boxes' `implicitWidth: label + 12`
    // convention (clock/volume/mic's own boxes).
    double box_w = text_w + 12.0;

    module_box_draw_rect(cr, x, box_top, box_w, box_height,
                          COLORS.mantle, COLORS.maroon, (double)BAR.module_border_width);
    module_box_draw_text_centered(cr, x, box_top, box_w, box_height, label, FONT.label, true, COLORS.maroon);

    if (rect) {
        rect->x = x;
        rect->y = box_top;
        rect->w = box_w;
        rect->h = box_height;
    }

    return x + box_w + BAR.item_spacing;
}

static const char *category_header(PrivacyCategory category) {
    switch (category) {
    case PRIVACY_CATEGORY_MIC: return "MICROPHONE";
    case PRIVACY_CATEGORY_WEBCAM: return "WEBCAM";
    case PRIVACY_CATEGORY_SCREENSHARE: return "SCREEN SHARE";
    }
    return "";
}

void privacy_handle_click(Output *output, double x, double y, uint32_t button) {
    if (button != BTN_RIGHT) {
        return;
    }
    OutputBoxRect *rect = NULL;
    for (int i = 0; i < MAX_TRACKED_OUTPUTS; i++) {
        if (g_rects[i].output == output) {
            rect = &g_rects[i];
            break;
        }
    }
    if (!rect || rect->w <= 0 || x < rect->x || x >= rect->x + rect->w || y < rect->y ||
        y >= rect->y + rect->h) {
        return;
    }

    // Grouped by category, in a fixed MIC/WEBCAM/SCREEN SHARE order --
    // matches the fixed enum order, not the order entries happen to be
    // stored in pipewire_client's node table.
    char body[2048];
    size_t len = 0;
    body[0] = '\0';

    int count = pipewire_client_entry_count();
    for (int cat = PRIVACY_CATEGORY_MIC; cat <= PRIVACY_CATEGORY_SCREENSHARE; cat++) {
        bool any_in_category = false;
        for (int i = 0; i < count; i++) {
            PrivacyCategory entry_category;
            char app_name[128];
            if (!pipewire_client_entry(i, &entry_category, app_name, sizeof(app_name))) {
                continue;
            }
            if (entry_category != (PrivacyCategory)cat) {
                continue;
            }
            if (!any_in_category) {
                if (len > 0 && len < sizeof(body) - 1) {
                    len += (size_t)snprintf(body + len, sizeof(body) - len, "\n");
                }
                len += (size_t)snprintf(body + len, sizeof(body) - len, "%s:\n", category_header((PrivacyCategory)cat));
                any_in_category = true;
            }
            len += (size_t)snprintf(body + len, sizeof(body) - len, "  %s\n", app_name);
            if (len >= sizeof(body) - 1) {
                break;
            }
        }
    }

    privacy_info_popup_open(output, body, rect->x, rect->y, rect->w, rect->h);
}
