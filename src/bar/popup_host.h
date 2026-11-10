#ifndef CUSTOMBAR_BAR_POPUP_HOST_H
#define CUSTOMBAR_BAR_POPUP_HOST_H

#include <stdbool.h>
#include <stdint.h>

#include "bar/popup_surface.h"
#include "wayland/context.h"

// The part of every bar dropdown that isn't about its contents: the dropdown
// surface itself, anchored under the box that opened it, and one full-screen
// click-catcher per output so a click anywhere else closes it (see
// docs/popups.md). Each dropdown used to carry its own copy of all of this --
// the same catcher arrays, create/destroy loops, surface-matching functions
// and margin math, eleven times over.
//
// A dropdown keeps a PopupHost inside its own state struct and draws into
// `host.popup` with popup_surface_render_now() as before.

// Sized generously (this machine has 2 monitors), matching every other
// per-output table in this project.
#define POPUP_MAX_OUTPUTS 8

// Which edge of the trigger box the dropdown lines up with. Boxes in the
// bar's left region (and the centered media box) open rightward from their
// left edge; right-region boxes open leftward from their right edge -- a
// right anchor for a left-region box would push the dropdown off-screen.
typedef enum {
    POPUP_ANCHOR_LEFT,
    POPUP_ANCHOR_RIGHT,
} PopupAnchorSide;

typedef struct {
    bool open;
    Output *output;          // where it's shown; NULL when closed
    PopupSurface *popup;     // the dropdown surface; NULL when closed

    // Kept from open time, so popup_host_resize() can recreate the surface
    // at the same spot.
    int top_margin, left_margin, right_margin;
    uint32_t anchor;

    PopupSurface *catchers[POPUP_MAX_OUTPUTS];
    int catcher_count;
} PopupHost;

// Opens a `width` x `height` dropdown under the box (box_x, box_y, box_w,
// box_h) on `output`, plus the click-catchers. The surface is created on
// LAYER_OVERLAY, strictly above the catchers (LAYER_TOP), so its own clicks
// reach it first. Returns false (host left closed) if the surface couldn't be
// created. The caller then renders into host->popup. The caller should close
// any previous instance first (a host that's still open is closed here).
bool popup_host_open(PopupHost *host, Output *output, PopupAnchorSide side, double box_x, double box_y,
                     double box_w, double box_h, int width, int height);

// Destroys the dropdown and its catchers. Safe on a closed host.
void popup_host_close(PopupHost *host);

// Replaces the dropdown surface with a new `width` x `height` one at the same
// spot -- for a dropdown whose content height changed while open. Recreated
// rather than resized in place: a zwlr_layer_surface_v1_set_size() that
// shrinks the surface made Hyprland unmap it (docs/popups.md). Returns false
// if the new surface couldn't be created; the caller should then close.
bool popup_host_resize(PopupHost *host, int width, int height);

// Whether `surface` is this host's dropdown / one of its catchers, for
// wayland/pointer.c's routing. False whenever the host is closed.
bool popup_host_is_menu_surface(const PopupHost *host, struct wl_surface *surface);
bool popup_host_is_catcher_surface(const PopupHost *host, struct wl_surface *surface);

#endif
