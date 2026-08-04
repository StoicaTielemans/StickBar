#ifndef CUSTOMBAR_MODULES_WORKSPACES_WORKSPACES_H
#define CUSTOMBAR_MODULES_WORKSPACES_WORKSPACES_H

#include <cairo.h>

#include "wayland/context.h"

// Re-queries Hyprland's current workspace/focus state (via hyprland/ipc_request.c)
// and caches it for rendering. Call once at startup, and again whenever
// hyprland/ipc_events.c sees an event that could have changed occupancy or focus.
void workspaces_refresh(void);

// Draws `output`'s workspace pills (only occupied-or-focused ids within that
// output's assigned range, see workspaces.c) left-to-right starting at (x, box_top),
// vertically centered in a box of height `box_height`. Also records each pill's
// screen-space rect for later hit-testing by workspaces_handle_click(). Returns the
// x coordinate immediately after the last pill drawn, i.e. where the next
// left-region widget should start.
double workspaces_render(cairo_t *cr, Output *output, double x, double box_top, double box_height);

// Hit-tests (x, y) against `output`'s most recently rendered pill rects (from the
// last workspaces_render() call for that output); if it lands inside one, dispatches
// a Hyprland workspace-focus request for that pill's workspace id. No-op if it
// doesn't land on any pill, or if this output hasn't been rendered yet.
void workspaces_handle_click(Output *output, double x, double y);

#endif
