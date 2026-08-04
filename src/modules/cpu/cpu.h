#ifndef CUSTOMBAR_MODULES_CPU_CPU_H
#define CUSTOMBAR_MODULES_CPU_CPU_H

#include <cairo.h>
#include <stdint.h>

#include "wayland/context.h"

// Draws the CPU module box left-anchored so its left edge sits at `x`,
// vertically spanning box_top..box_top+box_height -- same left-growing shape
// as modules/workspaces/workspaces.c's workspaces_render(), since this
// (like the reference SystemMonitorWidget it's split from) lives in the
// bar's left region, not the right. Records the box's screen-space rect for
// later cpu_handle_click(). Returns `x` advanced past this box plus
// BAR.item_spacing, for the next (further right) left-region module to
// anchor against.
double cpu_render(cairo_t *cr, Output *output, double x, double box_top, double box_height);

// Hit-tests (x, y) against `output`'s most recently rendered box. Right-click
// opens a read-only info dropdown (per-core usage, frequency, temperature,
// load average) via sysmon/sysmon_info_popup.c; left-click is a no-op (there's
// no mute-equivalent primary action for a system monitor widget). No-op if
// (x, y) doesn't land on the box.
void cpu_handle_click(Output *output, double x, double y, uint32_t button);

#endif
