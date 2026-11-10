#include "modules/privacy/privacy.h"

#include <linux/input-event-codes.h> // BTN_RIGHT
#include <stdio.h>
#include <string.h>

#include "config/config.h"
#include "config/glyphs.h"
#include "modules/privacy/privacy_info_popup.h"
#include "pipewire/pipewire_client.h"
#include "render/box_rects.h"
#include "render/module_box.h"

// Where this box was last drawn, per output (render/box_rects.h).
static BoxRectTable g_rects;

double privacy_render(cairo_t *cr, Output *output, double x, double box_top, double box_height) {
    bool mic = pipewire_client_mic_active();
    bool webcam = pipewire_client_webcam_active();
    bool screenshare = pipewire_client_screenshare_active();

    if (!mic && !webcam && !screenshare) {
        box_rect_clear(&g_rects, output); // collapsed: nothing to click
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

    return module_box_label(cr, &g_rects, output, MODULE_BOX_FROM_LEFT, x, box_top, box_height, label, COLORS.maroon);
}

void privacy_handle_click(Output *output, double x, double y, uint32_t button) {
    if (button != BTN_RIGHT) {
        return;
    }
    const BoxRect *rect = box_rect_hit(&g_rects, output, x, y); // NULL too while collapsed
    if (!rect) {
        return;
    }

    privacy_info_popup_open(output, rect->x, rect->y, rect->w, rect->h);
}
