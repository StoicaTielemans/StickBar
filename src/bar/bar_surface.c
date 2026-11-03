#include "bar/bar_surface.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    bool dirty_full;        // next opportunity should re-render the whole bar
    bool dirty_clock;       // next opportunity should re-render just the clock box
                            // (ignored if dirty_full is also set -- full covers it)
    struct wl_callback *frame_callback; // non-NULL while waiting for wl_surface.frame's `done`

    // Two private, CPU-only copies of the bar's pixels (same size/stride as the
    // shm buffers), swapped after every commit -- see do_render() for why:
    //   prev: exactly the frame last committed to the compositor
    //   next: scratch space the upcoming frame is drawn into
    // Kept separate from the two shm buffers on purpose: those ping-pong with the
    // compositor, so whichever one we get back holds a frame from *two* commits
    // ago, not the last one -- useless for "did anything change?" comparisons.
    uint8_t *prev;
    uint8_t *next;
    bool prev_valid;        // false until the first commit (and after a resize)
};

static void request_frame_callback(BarSurface *surf);

// Finds the smallest rectangle containing every pixel that differs between two
// same-sized frames. Returns false if they're identical. Rows are compared with
// memcmp() first (most rows of a clock-only update don't change at all), and only
// differing rows are scanned pixel by pixel -- cheap at 20 rows.
static bool diff_bounds(const uint8_t *a, const uint8_t *b, int width, int height, int stride,
                        int *out_x, int *out_y, int *out_w, int *out_h) {
    int min_x = width, max_x = -1, min_y = height, max_y = -1;
    for (int y = 0; y < height; y++) {
        const uint32_t *ra = (const uint32_t *)(a + (size_t)y * stride);
        const uint32_t *rb = (const uint32_t *)(b + (size_t)y * stride);
        if (memcmp(ra, rb, (size_t)width * 4) == 0) {
            continue;
        }
        if (y < min_y) min_y = y;
        max_y = y;
        for (int x = 0; x < min_x; x++) {
            if (ra[x] != rb[x]) {
                min_x = x;
                break;
            }
        }
        for (int x = width - 1; x > max_x; x--) {
            if (ra[x] != rb[x]) {
                max_x = x;
                break;
            }
        }
    }
    if (max_y < 0) {
        return false;
    }
    *out_x = min_x;
    *out_y = min_y;
    *out_w = max_x - min_x + 1;
    *out_h = max_y - min_y + 1;
    return true;
}

/*
 * Renders one frame and commits it -- but only what actually changed:
 *
 *   1. Draw into the private `next` frame: either the whole bar, or (when only the
 *      clock is dirty) a copy of `prev` with just the clock box repainted on top.
 *      The clock ticks every second while everything else changes far less often,
 *      so this skips re-laying-out every other module's text once a second.
 *   2. Compare `next` to `prev` (what the compositor currently shows). Identical
 *      -> no commit at all. Most triggers (a Hyprland event that didn't change any
 *      workspace pill, a PipeWire graph change unrelated to recording, a 5s poll
 *      landing on the same numbers) end here, so this one check covers every
 *      redraw source without each of them needing its own change detection.
 *   3. Otherwise copy `next` into a free shm buffer (the whole frame -- that buffer
 *      holds stale content from two commits ago) and commit with damage limited
 *      to the changed rectangle, so the compositor only re-uploads/recomposites
 *      that region (a few dozen px of clock digits on a typical tick).
 *
 * A new frame callback is requested only when actually committing --
 * wl_surface.frame only takes effect for the commit that follows it, and a skipped
 * frame has nothing for the compositor to pace.
 */
static void do_render(BarSurface *surf) {
    int width = surf->buffers.width, height = surf->buffers.height, stride = surf->buffers.stride;
    size_t frame_size = (size_t)stride * (size_t)height;

    cairo_surface_t *csurf = cairo_image_surface_create_for_data(
        surf->next, CAIRO_FORMAT_ARGB32, width, height, stride);
    cairo_t *cr = cairo_create(csurf);

    // Clock-only is only possible on top of a valid previous frame; anything else
    // (first frame, a full redraw requested, or the clock box changing size) falls
    // back to drawing everything.
    bool drawn = false;
    if (!surf->dirty_full && surf->prev_valid) {
        memcpy(surf->next, surf->prev, frame_size);
        cairo_surface_mark_dirty(csurf); // pixels changed behind cairo's back
        drawn = render_output_clock_only(cr, surf->output, width, height);
    }
    if (!drawn) {
        render_output(cr, surf->output, width, height);
    }

    cairo_destroy(cr);
    cairo_surface_flush(csurf);
    cairo_surface_destroy(csurf);

    int dx = 0, dy = 0, dw = width, dh = height;
    if (surf->prev_valid && !diff_bounds(surf->next, surf->prev, width, height, stride, &dx, &dy, &dw, &dh)) {
        // Nothing visible changed -- nothing to send.
        surf->dirty_full = false;
        surf->dirty_clock = false;
        return;
    }

    uint8_t *pixels = NULL;
    struct wl_buffer *buffer = shm_buffers_acquire(&surf->buffers, &pixels);
    if (!buffer) {
        // Both buffers are still held by the compositor. This should be rare given
        // the frame-callback throttle below; leave the dirty flags set so whichever
        // event fires next (a frame `done`, or another mark_dirty call) retries.
        return;
    }
    memcpy(pixels, surf->next, frame_size);

    wl_surface_attach(surf->wl_surface, buffer, 0, 0);
    wl_surface_damage_buffer(surf->wl_surface, dx, dy, dw, dh);
    request_frame_callback(surf);
    wl_surface_commit(surf->wl_surface);

    // `next` is now what's on screen.
    uint8_t *tmp = surf->prev;
    surf->prev = surf->next;
    surf->next = tmp;
    surf->prev_valid = true;

    surf->dirty_full = false;
    surf->dirty_clock = false;
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time_ms) {
    (void)time_ms;
    BarSurface *surf = data;
    wl_callback_destroy(callback);
    surf->frame_callback = NULL;
    if (surf->dirty_full || surf->dirty_clock) {
        do_render(surf); // re-arms its own new frame callback (if it commits)
    }
    // If not dirty (or do_render() found nothing changed), frame_callback simply stays NULL: the next bar_surface_mark_dirty()
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
            fprintf(stderr, "stickBar: failed to allocate shm buffers for a %ux%u bar surface\n", width, height);
            return;
        }
        // The private prev/next frames match the shm buffers' size and stride.
        // After a resize there's no valid "last committed frame" to compare
        // against, so the next render always commits in full.
        size_t frame_size = (size_t)surf->buffers.stride * (size_t)surf->buffers.height;
        free(surf->prev);
        free(surf->next);
        surf->prev = calloc(1, frame_size);
        surf->next = calloc(1, frame_size);
        surf->prev_valid = false;
        if (!surf->prev || !surf->next) {
            perror("stickBar: calloc(bar frame)");
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
        perror("stickBar: calloc(BarSurface)");
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
    free(surf->prev);
    free(surf->next);
    if (surf->layer_surface) {
        zwlr_layer_surface_v1_destroy(surf->layer_surface);
    }
    if (surf->wl_surface) {
        wl_surface_destroy(surf->wl_surface);
    }
    free(surf);
}

// Shared tail of both mark functions: render now if nothing's holding us back.
static void render_if_possible(BarSurface *surf) {
    if (!surf->configured || !surf->prev || !surf->next) {
        return; // nothing to draw yet; layer_surface_configure() triggers the first render
    }
    if (surf->frame_callback) {
        return; // frame_done() will notice the dirty flag and redraw when it fires
    }
    do_render(surf);
}

void bar_surface_mark_dirty(BarSurface *surf) {
    surf->dirty_full = true;
    render_if_possible(surf);
}

void bar_surface_mark_clock_dirty(BarSurface *surf) {
    surf->dirty_clock = true;
    render_if_possible(surf);
}
