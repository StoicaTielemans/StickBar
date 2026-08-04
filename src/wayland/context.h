#ifndef CUSTOMBAR_WAYLAND_CONTEXT_H
#define CUSTOMBAR_WAYLAND_CONTEXT_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

// Opaque forward declaration only. Output owns a pointer to a BarSurface (the real
// type lives in bar/bar_surface.h), but this header deliberately doesn't include
// that one -- the dependency direction throughout this project is wayland/* ->
// bar/*, never the reverse, so bar_surface.h is free to include context.h without
// a cycle.
typedef struct BarSurface BarSurface;

// Forward declaration so Output can hold an AppContext* back-pointer below; the
// full definition follows further down in this same file.
typedef struct AppContext AppContext;

// One tracked physical output (monitor). The Wayland protocol delivers a burst of
// wl_output events (geometry, mode, scale) followed by a single `done` event; we
// buffer that info here and only create the actual bar surface once `done` fires,
// since creating it earlier could act on a stale/incomplete size.
typedef struct Output {
    struct wl_output *wl_output;
    struct wl_surface *wl_surface;  // set by bar/bar_surface.c once created; lets
                                     // wayland/pointer.c match an incoming
                                     // wl_pointer.enter's surface back to its Output
                                     // by simple pointer comparison, no extra lookup
                                     // structure needed
    AppContext *app;          // back-pointer: the wl_output listener only receives
                               // the Output* passed as its user data, but its `done`
                               // handler needs the AppContext to create a bar surface
    uint32_t registry_name;   // wl_registry's global name, needed to match this Output
                               // up again when global_remove fires (unplug)
    int32_t width, height;    // from wl_output.mode, in output-local (unscaled) px
    int index;                 // 0-based order this output was bound in; this is what
                                // the workspace module uses to assign its 5-wide id
                                // range per output (see modules/workspaces)
    bool done_received;

    BarSurface *bar_surface;  // NULL until done_received flips this on

    struct Output *next;
} Output;

struct AppContext {
    struct wl_display *display;
    struct wl_registry *registry;

    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct zwlr_layer_shell_v1 *layer_shell;

    struct wl_pointer *pointer;

    Output *outputs;          // singly-linked list head
    int next_output_index;    // count of outputs bound so far; assigned to Output.index

    // Pointer hover state, updated by wayland/pointer.c's enter/motion/leave
    // handlers. `pointer_over_surface` is the raw wl_surface currently under the
    // pointer, regardless of what kind of surface it is; `pointer_over` is the
    // Output it resolves to *if* it's one of the bar's own surfaces (NULL for a
    // popup/click-catcher surface, which are never stored on any Output --
    // see wayland/pointer.c's button handler for how the two are used together
    // to route clicks to the right place).
    Output *pointer_over;
    struct wl_surface *pointer_over_surface;
    double pointer_x, pointer_y;
};

#endif
