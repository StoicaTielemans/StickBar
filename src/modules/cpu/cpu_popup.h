#ifndef CUSTOMBAR_MODULES_CPU_CPU_POPUP_H
#define CUSTOMBAR_MODULES_CPU_CPU_POPUP_H

#include <stdbool.h>
#include <wayland-client.h>

#include "wayland/context.h"

// The CPU box's right-click dropdown: a header (model, core/thread count,
// load pill), braille graphs of the last 5 minutes of usage and temperature,
// a per-thread grid (meter + % + clock, split into P-/E-cores on hybrid
// Intel chips), load/clock/uptime lines and the top CPU-using apps.
// Read-only, TOP+LEFT-anchored under the box -- the same shape as
// modules/gpu/gpu_popup.c, and like it, opening it closes the GPU and
// memory dropdowns (and they close it), since all three open at one spot.
void cpu_popup_open(Output *output, double box_x, double box_y, double box_w, double box_h);
void cpu_popup_close(void);

// Re-reads the dropdown-only details and repaints; call after each 5s sysmon
// poll. No-op when closed. The size stays as it was at open.
void cpu_popup_refresh_if_open(void);

// Call on every 1s clock tick: once, about a second after opening, it fills
// in the top-processes list (which needs two /proc scans). No-op otherwise.
void cpu_popup_tick(void);

bool cpu_popup_is_menu_surface(struct wl_surface *surface);
bool cpu_popup_is_catcher_surface(struct wl_surface *surface);

#endif
