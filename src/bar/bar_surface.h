#ifndef CUSTOMBAR_BAR_BAR_SURFACE_H
#define CUSTOMBAR_BAR_BAR_SURFACE_H

#include "wayland/context.h"

// (Re-declares the same incomplete-type typedef already forward-declared in
// context.h -- legal in C since it's an identical redeclaration, and lets any file
// that only needs bar_surface's own API include just this header.)
typedef struct BarSurface BarSurface;

// Creates the zwlr_layer_surface_v1 for `output` (which must already have
// done_received == true) and requests its first configure. The bar isn't actually
// paintable yet at this point -- nothing is rendered until the compositor's first
// `configure` event arrives and is acked (see bar_surface.c), since only that event
// tells us the width the compositor wants for a top+left+right-anchored surface.
BarSurface *bar_surface_create(AppContext *app, Output *output);

void bar_surface_destroy(BarSurface *surf);

// Marks this bar surface as needing a redraw. If no frame callback is currently
// pending, this renders+commits immediately and requests a new callback; otherwise
// the flag is picked up when the pending callback's `done` fires, so redraws never
// happen faster than the compositor is asking for.
void bar_surface_mark_dirty(BarSurface *surf);

#endif
