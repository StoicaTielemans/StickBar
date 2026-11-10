#include "bar/popup_host.h"

#include "config/config.h"

static PopupSurface *create_surface(PopupHost *host, int width, int height) {
    return popup_surface_create(host->output->app, host->output, width, height, host->top_margin, host->right_margin,
                                0, host->left_margin, host->anchor, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
}

// One catcher per currently-known output, not just the dropdown's own: on a
// multi-monitor setup a click on another monitor must close it too (the
// reference's HyprlandFocusGrab is compositor-wide).
static void create_catchers(PopupHost *host) {
    host->catcher_count = 0;
    for (Output *o = host->output->app->outputs; o && host->catcher_count < POPUP_MAX_OUTPUTS; o = o->next) {
        PopupSurface *catcher = popup_surface_create_catcher(host->output->app, o);
        if (catcher) {
            host->catchers[host->catcher_count++] = catcher;
        }
    }
}

bool popup_host_open(PopupHost *host, Output *output, PopupAnchorSide side, double box_x, double box_y,
                     double box_w, double box_h, int width, int height) {
    popup_host_close(host);
    host->output = output;

    // The popup's top edge sits at the box's bottom edge -- minus BAR.height:
    // Hyprland measures a same-client layer surface's margin from the edge of
    // the bar's already-reserved exclusive zone, so without the subtraction
    // the dropdown lands one bar height too low (docs/popups.md).
    host->top_margin = (int)(box_y + box_h) - BAR.height;
    if (side == POPUP_ANCHOR_LEFT) {
        host->left_margin = (int)box_x;
        host->right_margin = 0;
        host->anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
    } else {
        host->left_margin = 0;
        host->right_margin = output->width - (int)(box_x + box_w);
        if (host->right_margin < 0) {
            host->right_margin = 0;
        }
        host->anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    }

    host->popup = create_surface(host, width, height);
    if (!host->popup) {
        host->output = NULL;
        return false;
    }
    create_catchers(host);
    host->open = true;
    return true;
}

void popup_host_close(PopupHost *host) {
    if (host->popup) {
        popup_surface_destroy(host->popup);
        host->popup = NULL;
    }
    for (int i = 0; i < host->catcher_count; i++) {
        popup_surface_destroy(host->catchers[i]);
        host->catchers[i] = NULL;
    }
    host->catcher_count = 0;
    host->open = false;
    host->output = NULL;
}

bool popup_host_resize(PopupHost *host, int width, int height) {
    if (!host->open) {
        return false;
    }
    popup_surface_destroy(host->popup);
    host->popup = create_surface(host, width, height);
    return host->popup != NULL;
}

bool popup_host_is_menu_surface(const PopupHost *host, struct wl_surface *surface) {
    return host->open && host->popup && popup_surface_wl_surface(host->popup) == surface;
}

bool popup_host_is_catcher_surface(const PopupHost *host, struct wl_surface *surface) {
    if (!host->open) {
        return false;
    }
    for (int i = 0; i < host->catcher_count; i++) {
        if (popup_surface_wl_surface(host->catchers[i]) == surface) {
            return true;
        }
    }
    return false;
}
