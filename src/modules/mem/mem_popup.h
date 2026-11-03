#ifndef CUSTOMBAR_MODULES_MEM_MEM_POPUP_H
#define CUSTOMBAR_MODULES_MEM_MEM_POPUP_H

#include <stdbool.h>
#include <wayland-client.h>

#include "wayland/context.h"

// The memory box's right-click dropdown: a header (total, swap kind,
// pressure pill), braille graphs of the last 5 minutes of used % and swap %,
// an apps/cache/free composition bar, RAM/swap meters, zram/available/
// pressure lines and the top apps by private memory. Read-only,
// TOP+LEFT-anchored under the box -- the same shape as
// modules/cpu/cpu_popup.c, and like it, opening it closes the CPU and GPU
// dropdowns (and they close it), since all three open at one spot.
void mem_popup_open(Output *output, double box_x, double box_y, double box_w, double box_h);
void mem_popup_close(void);

// Re-reads the dropdown-only details and repaints; call after each 5s sysmon
// poll. No-op when closed. The size stays as it was at open.
void mem_popup_refresh_if_open(void);

bool mem_popup_is_menu_surface(struct wl_surface *surface);
bool mem_popup_is_catcher_surface(struct wl_surface *surface);

#endif
