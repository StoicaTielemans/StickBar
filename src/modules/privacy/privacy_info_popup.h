#ifndef CUSTOMBAR_MODULES_PRIVACY_PRIVACY_INFO_POPUP_H
#define CUSTOMBAR_MODULES_PRIVACY_PRIVACY_INFO_POPUP_H

#include <stdbool.h>
#include <wayland-client.h>

#include "wayland/context.h"

// Opens a read-only info popup showing `body` (grouped MICROPHONE/WEBCAM/
// SCREEN SHARE sections, one app name per line -- see modules/privacy/
// privacy.c's privacy_handle_click() for how it's built), left-anchored
// under the trigger box at (box_x, box_y, box_h) on `output` (box_w is
// unused -- only the box's left edge matters for a left-anchored popup) --
// same TOP+LEFT anchor math (including the BAR.height correction) as
// sysmon/sysmon_info_popup.c, since the privacy module lives in the bar's
// left region like CPU/GPU/memory. Always styled in COLORS.maroon (unlike
// sysmon_info_popup_open(), this popup has only the one caller/accent, so
// there's no need to parameterize it). Closes any previously open privacy
// info popup first (single-popup-at-a-time, same v1 simplification as every
// other popup in this project -- see CLAUDE.md's "Known limitations").
void privacy_info_popup_open(Output *output, const char *body,
                              double box_x, double box_y, double box_w, double box_h);

// Closes the currently open info popup, if any (no-op otherwise).
void privacy_info_popup_close(void);

// True if `surface` is the popup's own surface / one of its per-output
// click-catcher surfaces -- used by wayland/pointer.c to route button events.
// No motion/click handler pair here (this popup's content is read-only, so a
// click landing on the popup surface itself is simply absorbed).
bool privacy_info_popup_is_menu_surface(struct wl_surface *surface);
bool privacy_info_popup_is_catcher_surface(struct wl_surface *surface);

#endif
