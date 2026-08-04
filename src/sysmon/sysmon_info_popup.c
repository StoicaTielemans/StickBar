#include "sysmon/sysmon_info_popup.h"

#include <stdio.h>

#include "bar/popup_surface.h"
#include "render/module_box.h"

// Sized generously (this machine has 2 monitors), matching the same
// MAX_TRACKED_OUTPUTS convention used by modules/tray/tray_context_menu.c and
// audio/device_popup.c.
#define MAX_TRACKED_OUTPUTS 8
#define OUTER_MARGIN 8.0 // matches Config.bar.popupPadding, same as the other two popups
#define BODY_MAX_LEN 2048

typedef struct {
    bool open;
    Color accent;
    Output *output;
    PopupSurface *popup;
    char body[BODY_MAX_LEN];
    double popup_width, popup_height;

    PopupSurface *catchers[MAX_TRACKED_OUTPUTS];
    struct wl_surface *catcher_surfaces[MAX_TRACKED_OUTPUTS];
    int catcher_count;
} InfoPopupState;

static InfoPopupState g_popup;

static void draw_transparent(cairo_t *cr, void *user_data) {
    (void)user_data;
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

static void draw_popup(cairo_t *cr, void *user_data) {
    (void)user_data;
    module_box_draw_rect(cr, 0, 0, g_popup.popup_width, g_popup.popup_height,
                          COLORS.mantle, g_popup.accent, (double)BAR.module_border_width);
    // The popup's own size is computed (in open(), below) as exactly
    // text_size + OUTER_MARGIN*2, so centering `body` within
    // (popup_height - OUTER_MARGIN*2) here works out to the same position as
    // top-aligning it at OUTER_MARGIN would -- reusing draw_text_left's
    // existing vertical-centering math rather than adding a top-align variant.
    module_box_draw_text_left(cr, OUTER_MARGIN, OUTER_MARGIN, g_popup.popup_height - OUTER_MARGIN * 2.0,
                               g_popup.body, FONT.label, false, g_popup.accent);
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

void sysmon_info_popup_open(Output *output, Color accent, const char *body,
                             double box_x, double box_y, double box_w, double box_h) {
    (void)box_w; // left-anchored: only the box's left edge (box_x) matters, not its width
    sysmon_info_popup_close(); // at most one sysmon info popup open at a time

    g_popup.accent = accent;
    g_popup.output = output;
    snprintf(g_popup.body, sizeof(g_popup.body), "%s", body);

    double text_w, text_h;
    module_box_measure_text(g_popup.body, FONT.label, false, &text_w, &text_h);
    g_popup.popup_width = text_w + OUTER_MARGIN * 2.0;
    g_popup.popup_height = text_h + OUTER_MARGIN * 2.0;

    // Same top_margin math (including the BAR.height correction) as
    // modules/tray/tray_context_menu.c and audio/device_popup.c -- see
    // CLAUDE.md's "D-Bus / system tray" section for the reasoning. Anchored
    // TOP+LEFT with a left_margin here, unlike those two, because CPU/GPU/
    // memory live in the bar's *left* region: right-anchoring (as first
    // written) put the popup's right edge under the trigger box and grew it
    // leftward, which clipped off the left edge of the screen for a trigger
    // box already near x=0 -- a real bug the user caught, not a hypothetical.
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
    popup_surface_render_now(g_popup.popup, draw_popup, NULL);
}

void sysmon_info_popup_close(void) {
    if (!g_popup.open) {
        return;
    }
    popup_surface_destroy(g_popup.popup);
    g_popup.popup = NULL;
    destroy_catchers();
    g_popup.open = false;
    g_popup.output = NULL;
}

bool sysmon_info_popup_is_menu_surface(struct wl_surface *surface) {
    return g_popup.open && g_popup.popup && popup_surface_wl_surface(g_popup.popup) == surface;
}

bool sysmon_info_popup_is_catcher_surface(struct wl_surface *surface) {
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
