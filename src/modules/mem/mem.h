#ifndef CUSTOMBAR_MODULES_MEM_MEM_H
#define CUSTOMBAR_MODULES_MEM_MEM_H

#include <cairo.h>
#include <stdint.h>

#include "wayland/context.h"

// Draws the memory module box left-anchored so its left edge sits at `x`,
// vertically spanning box_top..box_top+box_height -- see modules/cpu/cpu.h
// for why this is left-growing rather than right-growing like the tray/
// clock/volume/mic boxes. Records the box's screen-space rect for later
// mem_handle_click(). Returns `x` advanced past this box plus
// BAR.item_spacing, for the next (further right) left-region module to
// anchor against.
double mem_render(cairo_t *cr, Output *output, double x, double box_top, double box_height);

// Hit-tests (x, y) against `output`'s most recently rendered box. Right-click
// opens a read-only info dropdown (used/free/swap in GB) via
// sysmon/sysmon_info_popup.c; left-click is a no-op. No-op if (x, y) doesn't
// land on the box.
void mem_handle_click(Output *output, double x, double y, uint32_t button);

#endif
