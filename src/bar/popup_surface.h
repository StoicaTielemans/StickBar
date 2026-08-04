#ifndef CUSTOMBAR_BAR_POPUP_SURFACE_H
#define CUSTOMBAR_BAR_POPUP_SURFACE_H

#include <cairo.h>
#include <stdint.h>

#include "wayland/context.h"

typedef struct PopupSurface PopupSurface; // opaque

// Creates a transient zwlr_layer_surface_v1 with an explicit size, anchor, and
// top/right margin -- unlike bar_surface.c's always-anchored-top/left/right,
// compositor-decides-width bar surface, this is meant for a short-lived surface
// positioned at an arbitrary point (a popup menu near some other bar content)
// or covering the whole output (a full-screen, invisible click-catcher) -- see
// modules/tray/tray_context_menu.c for both use cases. No exclusive zone is
// set. `layer` lets the caller request e.g. LAYER_OVERLAY (to sit above the bar
// and any click-catcher surface) instead of LAYER_TOP. Returns NULL on
// failure. Written generically (no tray-specific knowledge) so a later
// tooltip/popup module can reuse it directly.
PopupSurface *popup_surface_create(AppContext *app, Output *output, int width, int height,
                                    int top_margin, int right_margin, uint32_t anchor,
                                    enum zwlr_layer_shell_v1_layer layer);

// Renders once immediately via `draw(cr, user_data)` and commits -- no
// frame-callback throttling like bar_surface.c's do_render(), since a popup only
// redraws on discrete state changes (e.g. hover moving between menu entries, at
// most a handful of times per open), not continuously. A safe no-op if the
// surface isn't configured yet, or if both shm buffers are still held by the
// compositor (should be rare at this update rate).
void popup_surface_render_now(PopupSurface *surf, void (*draw)(cairo_t *cr, void *user_data), void *user_data);

// For matching this surface up against wl_pointer enter/leave events.
struct wl_surface *popup_surface_wl_surface(PopupSurface *surf);

void popup_surface_destroy(PopupSurface *surf);

#endif
