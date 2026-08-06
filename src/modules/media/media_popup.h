#ifndef CUSTOMBAR_MODULES_MEDIA_MEDIA_POPUP_H
#define CUSTOMBAR_MODULES_MEDIA_MEDIA_POPUP_H

#include <stdbool.h>
#include <wayland-client.h>

#include "config/config.h"
#include "wayland/context.h"

// Opens a dropdown listing every currently-tracked MPRIS player (not just
// the active one media.c renders in the bar) with prev/play-pause/next
// controls per row. Structurally the media-domain twin of
// audio/device_popup.c (same popup/click-catcher/outside-click-dismiss
// shape, same live "recompute on every relevant update while open" model --
// see media_popup_refresh_if_open() below) rather than
// sysmon/sysmon_info_popup.c's read-only twin, since this one is
// interactive. Anchored TOP+LEFT with box_x as the left_margin, same
// left-anchored math as sysmon/sysmon_info_popup.c -- chosen because the
// media widget lives in the bar's *center* region rather than flush against
// either edge, so growing the popup rightward from the trigger's left edge
// (rather than the tray/audio popups' own right-anchored growth) was judged
// the more reasonable default; no overflow-past-either-edge clamping is
// added, same documented limitation as every other popup in this bar.
void media_popup_open(Output *output, Color accent, double box_x, double box_y, double box_w, double box_h);
void media_popup_close(void);

bool media_popup_is_menu_surface(struct wl_surface *surface);
bool media_popup_is_catcher_surface(struct wl_surface *surface);

void media_popup_handle_motion(double x, double y);

// Landing on a row's prev/play-pause/next icon dispatches the matching MPRIS
// call for that row's own player; landing on the row's track text is a
// no-op. Deliberately doesn't close the popup afterward, matching
// audio/device_popup.c's own device_popup_handle_click()'s "stay open" choice
// -- a control click here isn't a selection that should dismiss the menu.
void media_popup_handle_click(double x, double y);

// Called after any MPRIS PropertiesChanged/NameOwnerChanged update (see
// main.c's D-Bus pump) so a live track change, play/pause toggle, or a
// player appearing/disappearing shows up immediately while the popup is
// open -- same "recompute unconditionally, gated internally by
// g_popup.open" pattern as audio/device_popup.c's
// device_popup_refresh_if_open().
void media_popup_refresh_if_open(void);

// Re-fetches every visible row's playback position (MPRIS's Position isn't
// push-notified, unlike everything else this popup shows) and redraws --
// a no-op unless the popup is open. Called once a second from main.c's
// existing clock timerfd handler, reusing that already-ticking timer rather
// than adding a new poll source just for this.
void media_popup_tick(void);

#endif
