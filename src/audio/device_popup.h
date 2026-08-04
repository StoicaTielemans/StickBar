#ifndef CUSTOMBAR_AUDIO_DEVICE_POPUP_H
#define CUSTOMBAR_AUDIO_DEVICE_POPUP_H

#include <stdbool.h>
#include <wayland-client.h>

#include "config/config.h"
#include "wayland/context.h"

// Opens the sink (is_sink=true) or source (is_sink=false) device dropdown,
// right-anchored under the module box at (box_x, box_y, box_w, box_h) on
// `output` -- same right-anchored-under-the-trigger positioning
// modules/tray/tray_context_menu.c already uses (both live in the bar's right
// region, so this reuses that already-verified anchor+margin math rather than
// the reference QML's own left-aligned-under-widget popup, a deliberate,
// documented deviation from strict visual parity). Styled in `accent`
// (COLORS.sky for volume, COLORS.blue for mic, matching each widget's own
// border/text color). Closes any previously open device popup first (at most
// one open at a time -- this module's state is a single static singleton
// shared between the sink and source dropdowns, same v1 simplification as
// tray_context_menu.c's own single-menu-at-a-time rule). Does *not* close the
// tray's own context menu if that's open instead -- the two are independent
// singletons; having both open at once is an undefended, rare v1 edge case,
// see CLAUDE.md's "Known limitations".
void device_popup_open(Output *output, bool is_sink, Color accent,
                        double box_x, double box_y, double box_w, double box_h);

// Closes the currently open device popup, if any (no-op otherwise).
void device_popup_close(void);

// Re-reads pulse_client's live cache and redraws, if a popup is currently
// open (no-op otherwise) -- called after every pulse_client_pump() so an
// external volume/mute/default-device change (another app, physical media
// keys) shows up live instead of only reflecting state as of the moment the
// popup opened. Safe to call unconditionally on every audio-cache update.
void device_popup_refresh_if_open(void);

// True if `surface` is the popup's own menu surface / one of its per-output
// click-catcher surfaces -- used by wayland/pointer.c to route
// enter/motion/button events, same split as tray_context_menu.c's pair.
bool device_popup_is_menu_surface(struct wl_surface *surface);
bool device_popup_is_catcher_surface(struct wl_surface *surface);

// Hover tracking within the open popup (updates which row is highlighted and
// redraws if it changed). No-op if no popup is currently open.
void device_popup_handle_motion(double x, double y);

// Click-to-trigger within the open popup: clicking the default-marker or the
// description sets that row's device as the default sink/source; clicking the
// mute icon toggles that row's mute -- neither closes the popup afterward
// (unlike the tray's menu), matching the reference's own hover-dismiss model
// where picking a device or muting one is expected to be followed by more
// interaction in the same dropdown. No-op if no popup is open or (x, y)
// doesn't land on a row.
void device_popup_handle_click(double x, double y);

#endif
