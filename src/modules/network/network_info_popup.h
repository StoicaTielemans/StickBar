#ifndef CUSTOMBAR_MODULES_NETWORK_NETWORK_INFO_POPUP_H
#define CUSTOMBAR_MODULES_NETWORK_NETWORK_INFO_POPUP_H

#include <stdbool.h>
#include <wayland-client.h>

#include "wayland/context.h"

// Opens a read-only dropdown for the default-route interface: a status
// header, a btop-style braille graph of the last 5 minutes of download/
// upload rate (network_client.c's history), a details grid (IP, gateway,
// MAC, MTU, link, totals) and the DNS servers as tags. Structurally the
// network-domain twin of sysmon/sysmon_info_popup.c (catchers, read-only
// clicks), but repainted on every 5s poll while open so the graph moves. Anchored TOP+RIGHT under the trigger's right edge (like
// audio/device_popup.c's and modules/tray/tray_context_menu.c's own popups)
// rather than sysmon_info_popup.c's TOP+LEFT, since the network widget lives
// in the bar's *right* region, not the left like CPU/GPU/memory.
void network_info_popup_open(Output *output, double box_x, double box_y, double box_w, double box_h);
void network_info_popup_close(void);

// Repaints the open popup with the latest poll (call right after
// network_client_poll()). No-op when closed. The size stays as it was at open.
void network_info_popup_refresh_if_open(void);

bool network_info_popup_is_menu_surface(struct wl_surface *surface);
bool network_info_popup_is_catcher_surface(struct wl_surface *surface);

#endif
