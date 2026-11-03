#ifndef CUSTOMBAR_MODULES_GPU_GPU_POPUP_H
#define CUSTOMBAR_MODULES_GPU_GPU_POPUP_H

#include <stdbool.h>
#include <wayland-client.h>

#include "wayland/context.h"

// The GPU box's right-click dropdown: a header (name, driver, P-state pill),
// btop-style braille graphs of the last 5 minutes of usage and temperature,
// VRAM/power/fan meters, clock and video-engine lines, and the top VRAM-using
// apps. Read-only (clicks on it are absorbed, a catcher click closes it),
// TOP+LEFT-anchored under the box like sysmon/sysmon_info_popup.c -- the GPU
// box lives in the bar's left region. Opening it closes the CPU/memory info
// popup and vice versa, since both drop down from the same spot.
void gpu_popup_open(Output *output, double box_x, double box_y, double box_w, double box_h);
void gpu_popup_close(void);

// Re-reads the dropdown-only NVML fields and repaints; call after each 5s
// sysmon poll. No-op when closed. The size stays as it was at open.
void gpu_popup_refresh_if_open(void);

bool gpu_popup_is_menu_surface(struct wl_surface *surface);
bool gpu_popup_is_catcher_surface(struct wl_surface *surface);

#endif
