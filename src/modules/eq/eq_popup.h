#ifndef CUSTOMBAR_MODULES_EQ_EQ_POPUP_H
#define CUSTOMBAR_MODULES_EQ_EQ_POPUP_H

#include <stdbool.h>
#include <wayland-client.h>

#include "wayland/context.h"

// Opens the EQ dropdown under the bar's EQ box: a header line (profile, gain
// mode, preamp), a full-size graph of the current curve, and one clickable
// row per offered profile. Clicking a row runs the script's `profile NAME`
// in the background (eq/eq_state.c) and leaves the popup open -- the row
// shows "applying..." until the script's state-file write arrives, at which
// point eq_popup_refresh_if_open() redraws the graph with the new curve.
// Anchored TOP+RIGHT under the trigger's right edge, like the other right-
// region dropdowns (audio/device_popup.c, network_info_popup.c).
void eq_popup_open(Output *output, double box_x, double box_y, double box_w, double box_h);
void eq_popup_close(void);

// Re-renders with the latest eq_state snapshot; cheap no-op if closed.
// Called from main.c whenever the state file changes.
void eq_popup_refresh_if_open(void);

bool eq_popup_is_menu_surface(struct wl_surface *surface);
bool eq_popup_is_catcher_surface(struct wl_surface *surface);
void eq_popup_handle_motion(double x, double y);
void eq_popup_handle_click(double x, double y);

#endif
