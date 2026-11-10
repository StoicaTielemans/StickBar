#ifndef CUSTOMBAR_BAR_STRIP_SURFACE_H
#define CUSTOMBAR_BAR_STRIP_SURFACE_H

#include <cairo.h>
#include <stdbool.h>

#include "wayland/context.h"

// A small wl_subsurface drawn and committed on its own, on top of a parent
// surface (the bar, or a popup) -- for the spectrum bars, the only thing in
// this project that animates continuously. Redrawing them as part of the
// parent meant copying and diffing the whole 1920x20 bar (or the whole popup)
// every frame just to change a few hundred pixels; a separate surface only
// ever touches its own few KB, and the parent isn't touched at all. See
// docs/modules/media.md for the measured difference.
//
// Desynchronized, so its commits show without a parent commit. Its position,
// though, is parent state per the protocol: strip_surface_place() takes effect
// with the parent's next commit -- fine here, since the strip only moves when
// the parent's layout changes, which always commits the parent anyway.
//
// Takes no pointer input (empty input region), so clicks on it land on the
// parent exactly as if it weren't there.
typedef struct StripSurface StripSurface;

// NULL if the compositor has no wl_subcompositor (callers then draw nothing).
StripSurface *strip_surface_create(AppContext *app, struct wl_surface *parent);
void strip_surface_destroy(StripSurface *strip);

// The parent it was created on -- callers compare this to detect that the
// parent surface was replaced (output re-plugged, popup recreated) and the
// strip has to be recreated with it.
struct wl_surface *strip_surface_parent(StripSurface *strip);

// Sets the position (parent-local px) and size. A size change reallocates the
// strip's two small shm buffers; the next strip_surface_draw() uses them.
void strip_surface_place(StripSurface *strip, int x, int y, int w, int h);

// Paints the whole strip with `draw` and commits it -- unless the previous
// frame hasn't been presented yet (frame callback still pending), in which case
// it's done when that callback arrives. So it never outpaces the compositor,
// and costs nothing while the compositor isn't showing it at all. `draw`/
// `user_data` must stay valid until then (static data, in practice). Shows the
// strip if hidden.
void strip_surface_draw(StripSurface *strip, void (*draw)(cairo_t *cr, int w, int h, void *user_data),
                        void *user_data);

// Unmaps it (no buffer). The next strip_surface_draw() maps it again.
void strip_surface_hide(StripSurface *strip);

// Shown, and not waiting on the compositor -- i.e. a draw now would really
// reach the screen. The visualizer skips its analysis when no strip is ready.
bool strip_surface_ready(StripSurface *strip);

#endif
