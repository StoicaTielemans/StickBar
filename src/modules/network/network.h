#ifndef CUSTOMBAR_MODULES_NETWORK_NETWORK_H
#define CUSTOMBAR_MODULES_NETWORK_NETWORK_H

#include <cairo.h>
#include <stddef.h>
#include <stdint.h>

#include "wayland/context.h"

// Draws the default-route interface's module box right-anchored so its
// right edge sits at `right_edge_x`, vertically spanning
// box_top..box_top+box_height -- same shape as modules/volume/volume.c's
// volume_render(), since this lives in the bar's right region between
// the EQ module and Clock (otherwise matching the reference Bar.qml's own rightLayout order:
// Mic, Volume, Network, Clock, Tray). Records the box's screen-space rect
// for later network_handle_click(). Returns the box's own left edge minus
// BAR.item_spacing, for the next (further left) right-region module to
// anchor against with a gap.
double network_render(cairo_t *cr, Output *output, double right_edge_x, double box_top, double box_height);

// Hit-tests (x, y) against `output`'s most recently rendered box.
// Right-click opens a read-only info dropdown (interface, IP/gateway/MAC,
// DNS, rx/tx rate and totals) via modules/network/network_info_popup.c;
// left-click is a no-op -- there's no obvious single primary action for a
// network status widget, same reasoning modules/cpu/cpu.h already documents
// for its own right-click-only convention (this module is a Wi-Fi-and-wired
// twin of that same shape, not a control widget like volume/mic). No-op if
// (x, y) doesn't land on the box.
void network_handle_click(Output *output, double x, double y, uint32_t button);

// The box's icon: no default route at all, else disconnected/connected,
// each split on Wi-Fi vs. Ethernet -- NetworkWidget.qml's icon(). Shared with
// the dropdown's header.
uint32_t network_status_glyph(void);

// "1.2 MB/s" / "340.0 KB/s" / "12 B/s" -- NetworkMonitor.qml's formatRate()
// thresholds and precision. Shared with the dropdown.
void network_format_rate(double bytes_per_sec, char *out, size_t out_size);

#endif
