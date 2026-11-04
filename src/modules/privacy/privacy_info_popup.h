#ifndef CUSTOMBAR_MODULES_PRIVACY_PRIVACY_INFO_POPUP_H
#define CUSTOMBAR_MODULES_PRIVACY_PRIVACY_INFO_POPUP_H

#include <stdbool.h>
#include <wayland-client.h>

#include "wayland/context.h"

// Opens a read-only dropdown naming what's using the mic/webcam/screen: a
// header with an `in use` pill, then one card per active category listing its
// apps, snapshotted from pipewire_client at open time. Left-anchored
// under the trigger box at (box_x, box_y, box_h) on `output` (box_w is
// unused -- only the box's left edge matters for a left-anchored popup) --
// same TOP+LEFT anchor math (including the BAR.height correction) as
// modules/cpu/cpu_popup.c, since the privacy module lives in the bar's
// left region like CPU/GPU/memory. Always styled in COLORS.maroon (it has only
// the one caller/accent, so there's no need to parameterize it). Closes any previously open privacy
// info popup first (single-popup-at-a-time, same v1 simplification as every
// other popup in this project -- see docs/popups.md).
void privacy_info_popup_open(Output *output, double box_x, double box_y, double box_w, double box_h);

// Closes the currently open info popup, if any (no-op otherwise).
void privacy_info_popup_close(void);

// True if `surface` is the popup's own surface / one of its per-output
// click-catcher surfaces -- used by wayland/pointer.c to route button events.
// No motion/click handler pair here (this popup's content is read-only, so a
// click landing on the popup surface itself is simply absorbed).
bool privacy_info_popup_is_menu_surface(struct wl_surface *surface);
bool privacy_info_popup_is_catcher_surface(struct wl_surface *surface);

// Test-control hooks (testing/test_control.c): the streams listed (count is -1
// if closed), with each one's app name and category title.
int privacy_info_popup_test_entry_count(void);
const char *privacy_info_popup_test_entry_name(int i);
const char *privacy_info_popup_test_entry_category(int i);

#endif
