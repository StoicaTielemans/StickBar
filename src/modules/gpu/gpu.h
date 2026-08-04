#ifndef CUSTOMBAR_MODULES_GPU_GPU_H
#define CUSTOMBAR_MODULES_GPU_GPU_H

#include <cairo.h>
#include <stdint.h>

#include "wayland/context.h"

// Draws the GPU module box left-anchored so its left edge sits at `x`,
// vertically spanning box_top..box_top+box_height -- see modules/cpu/cpu.h
// for why this is left-growing rather than right-growing like the tray/
// clock/volume/mic boxes. Shows "--" instead of a percent if no NVIDIA GPU
// has ever been successfully queried this run (see sysmon_gpu_available()).
// Records the box's screen-space rect for later gpu_handle_click(). Returns
// `x` advanced past this box plus BAR.item_spacing, for the next (further
// right) left-region module to anchor against.
double gpu_render(cairo_t *cr, Output *output, double x, double box_top, double box_height);

// Hit-tests (x, y) against `output`'s most recently rendered box. Right-click
// opens a read-only info dropdown (utilization, VRAM, power, fan, pstate) via
// sysmon/sysmon_info_popup.c; left-click is a no-op. No-op if (x, y) doesn't
// land on the box.
void gpu_handle_click(Output *output, double x, double y, uint32_t button);

#endif
