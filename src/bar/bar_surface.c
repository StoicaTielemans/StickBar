#include "bar/bar_surface.h"

#include <stdio.h>
#include <stdlib.h>

#include "config/config.h"
#include "render/render.h"
#include "wayland/shm_buffer.h"

// Full per-output bar surface state. Opaque outside this file (see bar_surface.h) --
// everything else interacts with it only through bar_surface_create/_destroy/_mark_dirty.
struct BarSurface {
    AppContext *app;
    Output *output;

    struct wl_surface *wl_surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    ShmBuffers buffers;

    bool configured;       // true once the compositor's first `configure` has been acked
    bool dirty;             // true if the next opportunity should render+commit a new frame
    struct wl_callback *frame_callback; // non-NULL while waiting for wl_surface.frame's `done`
};

static void request_frame_callback(BarSurface *surf);

// Renders one frame into whichever shm buffer isn't currently held by the
// compositor, then attaches/damages/commits it and arms a new frame callback so the
// *next* redraw (if any) waits for the compositor's own pacing instead of firing
// immediately. Requesting the callback before committing is required -- wl_surface.frame
// only takes effect for the commit that follows it.
static void do_render(BarSurface *surf) {
    uint8_t *pixels = NULL;
    struct wl_buffer *buffer = shm_buffers_acquire(&surf->buffers, &pixels);
    if (!buffer) {
        // Both buffers are still held by the compositor. This should be rare given
        // the frame-callback throttle below; leave `dirty` set so whichever event
        // fires next (a frame `done`, or another mark_dirty call) retries.
        return;
    }

    cairo_surface_t *csurf = cairo_image_surface_create_for_data(
        pixels, CAIRO_FORMAT_ARGB32, surf->buffers.width, surf->buffers.height, surf->buffers.stride);
    cairo_t *cr = cairo_create(csurf);

    render_output(cr, surf->output, surf->buffers.width, surf->buffers.height);

    cairo_destroy(cr);
    cairo_surface_destroy(csurf);

    wl_surface_attach(surf->wl_surface, buffer, 0, 0);
    wl_surface_damage_buffer(surf->wl_surface, 0, 0, surf->buffers.width, surf->buffers.height);
    request_frame_callback(surf);
    wl_surface_commit(surf->wl_surface);

    surf->dirty = false;
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time_ms) {
    (void)time_ms;
    BarSurface *surf = data;
    wl_callback_destroy(callback);
    surf->frame_callback = NULL;
    if (surf->dirty) {
        do_render(surf); // re-arms its own new frame callback
    }
    // If not dirty, frame_callback simply stays NULL: the next bar_surface_mark_dirty()
    // call will see that and redraw immediately instead of waiting on a callback
    // that would otherwise never come.
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static void request_frame_callback(BarSurface *surf) {
    surf->frame_callback = wl_surface_frame(surf->wl_surface);
    wl_callback_add_listener(surf->frame_callback, &frame_listener, surf);
}

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *layer_surface,
                                     uint32_t serial, uint32_t width, uint32_t height) {
    BarSurface *surf = data;
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);

    bool size_changed = !surf->configured || (int)width != surf->buffers.width || (int)height != surf->buffers.height;
    if (size_changed) {
        if (!shm_buffers_resize(&surf->buffers, surf->app->shm, (int)width, (int)height)) {
            fprintf(stderr, "customBar: failed to allocate shm buffers for a %ux%u bar surface\n", width, height);
            return;
        }
    }

    surf->configured = true;
    bar_surface_mark_dirty(surf);
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *layer_surface) {
    (void)layer_surface;
    BarSurface *surf = data;
    // The compositor is telling us this surface is gone (e.g. layer-shell support
    // was withdrawn). Save the owning Output before destroying surf, since surf
    // itself won't be valid to read from after bar_surface_destroy() returns.
    Output *output = surf->output;
    bar_surface_destroy(surf);
    if (output) {
        output->bar_surface = NULL;
        output->wl_surface = NULL;
    }
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

BarSurface *bar_surface_create(AppContext *app, Output *output) {
    BarSurface *surf = calloc(1, sizeof(BarSurface));
    if (!surf) {
        perror("customBar: calloc(BarSurface)");
        return NULL;
    }
    surf->app = app;
    surf->output = output;

    surf->wl_surface = wl_compositor_create_surface(app->compositor);
    // Lets wayland/pointer.c match a wl_pointer.enter's surface back to this Output
    // by plain pointer comparison, without needing a separate lookup structure.
    output->wl_surface = surf->wl_surface;

    surf->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        app->layer_shell, surf->wl_surface, output->wl_output,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP, "custombar");

    zwlr_layer_surface_v1_set_size(surf->layer_surface, 0, (uint32_t)BAR.height);
    zwlr_layer_surface_v1_set_anchor(surf->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(surf->layer_surface, BAR.height);
    zwlr_layer_surface_v1_set_keyboard_interactivity(surf->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

    zwlr_layer_surface_v1_add_listener(surf->layer_surface, &layer_surface_listener, surf);

    // The layer-shell protocol requires committing once with no buffer attached
    // before the compositor will send the first `configure` -- attaching a buffer
    // is only valid after that first configure has been acked (layer_surface_configure
    // above is what actually allocates buffers and renders the first frame).
    wl_surface_commit(surf->wl_surface);

    return surf;
}

void bar_surface_destroy(BarSurface *surf) {
    if (!surf) {
        return;
    }
    if (surf->frame_callback) {
        wl_callback_destroy(surf->frame_callback);
    }
    shm_buffers_destroy(&surf->buffers);
    if (surf->layer_surface) {
        zwlr_layer_surface_v1_destroy(surf->layer_surface);
    }
    if (surf->wl_surface) {
        wl_surface_destroy(surf->wl_surface);
    }
    free(surf);
}

void bar_surface_mark_dirty(BarSurface *surf) {
    surf->dirty = true;
    if (!surf->configured) {
        return; // nothing to draw yet; layer_surface_configure() triggers the first render
    }
    if (surf->frame_callback) {
        return; // frame_done() will notice `dirty` and redraw when it fires
    }
    do_render(surf);
}
