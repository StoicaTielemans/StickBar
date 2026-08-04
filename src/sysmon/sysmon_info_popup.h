#ifndef CUSTOMBAR_SYSMON_SYSMON_INFO_POPUP_H
#define CUSTOMBAR_SYSMON_SYSMON_INFO_POPUP_H

#include <stdbool.h>
#include <wayland-client.h>

#include "config/config.h"
#include "wayland/context.h"

// Opens a read-only info popup showing `body` (embedded '\n's render as real
// line breaks via Pango, not just measured as one long line -- see
// render/module_box.c's own note on why Pango is used over cairo's toy text
// API), left-anchored under the trigger box at (box_x, box_y, box_h) on
// `output` (box_w is unused -- only the box's left edge matters for a
// left-anchored popup) -- same top_margin math (including the BAR.height
// correction) as modules/tray/tray_context_menu.c and audio/device_popup.c,
// but anchored TOP+LEFT rather than TOP+RIGHT, since CPU/GPU/memory live in
// the bar's left region and a right-anchored popup there would grow off the
// left edge of the screen. Styled in `accent`. Closes any previously open
// sysmon info popup first
// (shared singleton across the CPU/GPU/memory modules, same v1 simplification
// as audio/device_popup.c's own single-popup-at-a-time rule for volume/mic --
// and, like that module, doesn't coordinate with the tray's or the audio
// popup's own open/close state either; see CLAUDE.md's "Known limitations").
void sysmon_info_popup_open(Output *output, Color accent, const char *body,
                             double box_x, double box_y, double box_w, double box_h);

// Closes the currently open info popup, if any (no-op otherwise).
void sysmon_info_popup_close(void);

// True if `surface` is the popup's own surface / one of its per-output
// click-catcher surfaces -- used by wayland/pointer.c to route button events,
// same split as tray_context_menu.c's and device_popup.c's own pairs. There's
// no motion/click handler pair here (unlike those two) -- this popup's
// content is read-only, so a click landing on the popup surface itself is
// simply absorbed (see wayland/pointer.c) rather than dispatched anywhere.
bool sysmon_info_popup_is_menu_surface(struct wl_surface *surface);
bool sysmon_info_popup_is_catcher_surface(struct wl_surface *surface);

#endif
