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

// Marks every currently-known output's bar surface dirty (needs a redraw). Every
// event source that can change the bar calls this rather than working out which
// output(s) it affects -- a redraw that changes nothing on an output isn't
// committed there anyway (see bar/bar_surface.c's do_render()).
void output_mark_all_dirty(AppContext *app);

// Same, but only the clock box needs repainting -- used by the 1-second clock
// tick so it doesn't re-render every other module each second
// (see bar_surface_mark_clock_dirty()).
void output_mark_all_clock_dirty(AppContext *app);

// Draws every output whose bar was marked dirty since the last flush -- once
// per event-loop iteration, from main.c, right before waiting for events.
void output_flush_dirty(AppContext *app);

#endif
