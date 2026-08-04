#ifndef CUSTOMBAR_WAYLAND_OUTPUT_H
#define CUSTOMBAR_WAYLAND_OUTPUT_H

#include "wayland/context.h"

// Binds a newly-advertised wl_output global and starts tracking it. The bar surface
// for this output isn't created yet -- that happens once the output's `done` event
// confirms all of its geometry/mode/scale info has arrived (see Output.done_received
// in context.h).
void output_add(AppContext *app, uint32_t name, uint32_t version);

// Tears down and frees the Output matching registry global `name` (called from
// wl_registry's global_remove, e.g. on monitor unplug), destroying its bar surface
// first if one had been created.
void output_remove(AppContext *app, uint32_t name);

// Marks every currently-known output's bar surface dirty (needs a redraw). Used by
// the clock tick and by Hyprland IPC event handling, neither of which know or care
// which specific output(s) are actually affected -- it's simpler and cheap enough at
// this event rate to just redraw everything.
void output_mark_all_dirty(AppContext *app);

#endif
