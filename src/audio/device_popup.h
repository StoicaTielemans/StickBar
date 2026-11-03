#ifndef CUSTOMBAR_AUDIO_DEVICE_POPUP_H
#define CUSTOMBAR_AUDIO_DEVICE_POPUP_H

#include <stdbool.h>
#include <stdint.h>
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

// Hover tracking within the open popup (which card / chip is highlighted),
// and while a volume drag is in progress, keeps setting that device's volume
// from the pointer's x (Wayland's implicit grab keeps delivering motion to
// this surface while the button is held, same as audio/mixer_popup.c).
// No-op if no popup is currently open.
void device_popup_handle_motion(double x, double y);

// Left press within the open popup: on a card's volume meter, sets that
// device's volume to the pressed position and starts a drag; on its mute
// chip, toggles mute; anywhere else on the card, makes it the default
// sink/source. Never closes the popup (unlike the tray's menu) -- picking a
// device or muting one is usually followed by more interaction here.
void device_popup_handle_press(double x, double y, uint32_t button);

// Ends a volume drag, if one is in progress. Safe to call on every release.
void device_popup_handle_release(void);

// Scrolling over a card steps that device's volume by
// PULSE_VOLUME_STEP_PERCENT (direction > 0 = up/louder).
void device_popup_handle_scroll(double x, double y, int direction);

// Test-control hooks (testing/test_control.c): the number of cards shown (-1
// if closed), and the popup-local point of a named device's part -- "body",
// "mute", or "meter" at `frac` (0..1) of its width. Not used by the bar itself.
int device_popup_test_card_count(void);
bool device_popup_test_target(const char *description, const char *part, double frac, double *x, double *y);

#endif
