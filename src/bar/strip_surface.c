#include "bar/strip_surface.h"

#include <stdio.h>
#include <stdlib.h>

#include "wayland/shm_buffer.h"

struct StripSurface {
    AppContext *app;
    struct wl_surface *parent;
    struct wl_surface *surface;
    struct wl_subsurface *subsurface;
    ShmBuffers buffers; // two tiny buffers, ping-ponged like bar_surface.c's
    int w, h;           // 0x0 until the first strip_surface_place()
    bool shown;         // a buffer is attached (mapped)

    struct wl_callback *frame_callback; // non-NULL while the last commit isn't presented yet
    bool dirty;                          // a draw arrived while waiting; do it on `done`
    void (*draw)(cairo_t *cr, int w, int h, void *user_data);
    void *user_data;
};

static void do_draw(StripSurface *strip);

static void frame_done(void *data, struct wl_callback *callback, uint32_t time_ms) {
    (void)time_ms;
    StripSurface *strip = data;
    wl_callback_destroy(callback);
    strip->frame_callback = NULL;
    if (strip->dirty) {
        do_draw(strip);
    }
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static void do_draw(StripSurface *strip) {
    strip->dirty = false;
    if (!strip->draw || strip->w <= 0 || strip->h <= 0) {
        return;
    }
    uint8_t *pixels = NULL;
    struct wl_buffer *buffer = shm_buffers_acquire(&strip->buffers, &pixels);
    if (!buffer) {
        // Both still held by the compositor -- rare behind the frame-callback
        // throttle. The next strip_surface_draw() call retries.
        return;
    }

    cairo_surface_t *csurf = cairo_image_surface_create_for_data(pixels, CAIRO_FORMAT_ARGB32, strip->buffers.width,
                                                                 strip->buffers.height, strip->buffers.stride);
    cairo_t *cr = cairo_create(csurf);
    strip->draw(cr, strip->w, strip->h, strip->user_data);
    cairo_destroy(cr);
    cairo_surface_flush(csurf);
    cairo_surface_destroy(csurf);

    wl_surface_attach(strip->surface, buffer, 0, 0);
    wl_surface_damage_buffer(strip->surface, 0, 0, strip->w, strip->h);
    strip->frame_callback = wl_surface_frame(strip->surface);
    wl_callback_add_listener(strip->frame_callback, &frame_listener, strip);
    wl_surface_commit(strip->surface);
    strip->shown = true;
}

StripSurface *strip_surface_create(AppContext *app, struct wl_surface *parent) {
    if (!app->subcompositor || !parent) {
        return NULL;
    }
    StripSurface *strip = calloc(1, sizeof(StripSurface));
    if (!strip) {
        perror("stickBar: calloc(StripSurface)");
        return NULL;
    }
    strip->app = app;
    strip->parent = parent;
    strip->surface = wl_compositor_create_surface(app->compositor);
    strip->subsurface = wl_subcompositor_get_subsurface(app->subcompositor, strip->surface, parent);
    // Desync: the strip's own commits apply immediately, not on the parent's
    // next commit -- the whole point (the parent isn't committed per frame).
    wl_subsurface_set_desync(strip->subsurface);

    // An empty input region: pointer events fall through to the parent (the
    // dropdown), exactly as if the graph weren't a separate surface.
    struct wl_region *region = wl_compositor_create_region(app->compositor);
    wl_surface_set_input_region(strip->surface, region);
    wl_region_destroy(region);
    return strip;
}

void strip_surface_destroy(StripSurface *strip) {
    if (!strip) {
        return;
    }
    if (strip->frame_callback) {
        wl_callback_destroy(strip->frame_callback);
    }
    shm_buffers_destroy(&strip->buffers);
    wl_subsurface_destroy(strip->subsurface);
    wl_surface_destroy(strip->surface);
    free(strip);
}

struct wl_surface *strip_surface_parent(StripSurface *strip) {
    return strip->parent;
}

void strip_surface_place(StripSurface *strip, int x, int y, int w, int h) {
    wl_subsurface_set_position(strip->subsurface, x, y);
    if (w != strip->w || h != strip->h) {
        if (!shm_buffers_resize(&strip->buffers, strip->app->shm, w, h)) {
            fprintf(stderr, "stickBar: failed to allocate a %dx%d strip buffer\n", w, h);
            strip->w = strip->h = 0;
            return;
        }
        strip->w = w;
        strip->h = h;
    }
}

void strip_surface_draw(StripSurface *strip, void (*draw)(cairo_t *cr, int w, int h, void *user_data),
                        void *user_data) {
    strip->draw = draw;
    strip->user_data = user_data;
    if (strip->frame_callback) {
        strip->dirty = true; // frame_done() draws it
        return;
    }
    do_draw(strip);
}

void strip_surface_hide(StripSurface *strip) {
    if (!strip->shown) {
        return;
    }
    if (strip->frame_callback) {
        wl_callback_destroy(strip->frame_callback);
        strip->frame_callback = NULL;
    }
    strip->dirty = false;
    wl_surface_attach(strip->surface, NULL, 0, 0);
    wl_surface_commit(strip->surface);
    strip->shown = false;
}

bool strip_surface_ready(StripSurface *strip) {
    return strip->shown && !strip->frame_callback;
}
