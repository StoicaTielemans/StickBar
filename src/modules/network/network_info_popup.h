#ifndef CUSTOMBAR_MODULES_NETWORK_NETWORK_INFO_POPUP_H
#define CUSTOMBAR_MODULES_NETWORK_NETWORK_INFO_POPUP_H

#include <stdbool.h>
#include <wayland-client.h>

#include "wayland/context.h"

// Opens a read-only dropdown showing the default-route interface's details
// (matches NetworkTooltip.qml's own NetworkMonitor.tooltip text) --
// structurally the network-domain twin of sysmon/sysmon_info_popup.c: same
// single-text-block, snapshot-at-open (not live-refreshed while open)
// content. Anchored TOP+RIGHT under the trigger's right edge (like
// audio/device_popup.c's and modules/tray/tray_context_menu.c's own popups)
// rather than sysmon_info_popup.c's TOP+LEFT, since the network widget lives
// in the bar's *right* region, not the left like CPU/GPU/memory.
void network_info_popup_open(Output *output, double box_x, double box_y, double box_w, double box_h);
void network_info_popup_close(void);

bool network_info_popup_is_menu_surface(struct wl_surface *surface);
bool network_info_popup_is_catcher_surface(struct wl_surface *surface);

#endif
