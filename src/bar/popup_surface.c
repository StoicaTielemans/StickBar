#include "bar/popup_surface.h"

#include <stdio.h>
#include <stdlib.h>

#include "wayland/shm_buffer.h"

struct PopupSurface {
    AppContext *app;
    struct wl_surface *wl_surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    ShmBuffers buffers;
    bool configured; // true once the compositor's first `configure` has been acked

    // Set for popup_surface_create_catcher() surfaces when the compositor has
    // wp_viewporter: the surface is then backed by a 1x1 buffer that this
    // viewport stretches to the configured (full-output) size. NULL for every
    // ordinary popup, and for catchers on a compositor without viewporter
    // (they fall back to a full-size transparent buffer).
    struct wp_viewport *viewport;

    // The most recently requested draw callback, cached rather than only ever
    // used immediately -- popup_surface_render_now() is typically called
    // synchronously right after popup_surface_create() (e.g. from a click
    // handler), long before the compositor's first `configure` has actually
    // been processed by this process's own Wayland dispatch (which only
    // happens later, back in main.c's event loop). Caching it here lets
    // layer_surface_configure() perform that first real paint itself once
    // `configured` actually becomes true, instead of the caller's first
    // render_now() call silently doing nothing and nothing ever repainting.
    void (*pending_draw)(cairo_t *cr, void *user_data);
    void *pending_user_data;
    bool has_pending_draw;
};

static void do_render(PopupSurface *surf) {
    uint8_t *pixels = NULL;
    struct wl_buffer *buffer = shm_buffers_acquire(&surf->buffers, &pixels);
    if (!buffer) {
        // Both buffers still held by the compositor -- should be rare given how
        // infrequently a popup actually redraws; simply skip this one. The
        // pending draw callback stays cached, so a later render_now() call
        // (even with identical arguments) will retry.
        return;
    }

    cairo_surface_t *csurf = cairo_image_surface_create_for_data(
        pixels, CAIRO_FORMAT_ARGB32, surf->buffers.width, surf->buffers.height, surf->buffers.stride);
    cairo_t *cr = cairo_create(csurf);

    surf->pending_draw(cr, surf->pending_user_data);

    cairo_destroy(cr);
    cairo_surface_destroy(csurf);

    wl_surface_attach(surf->wl_surface, buffer, 0, 0);
    wl_surface_damage_buffer(surf->wl_surface, 0, 0, surf->buffers.width, surf->buffers.height);
    wl_surface_commit(surf->wl_surface);
}

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *layer_surface,
                                     uint32_t serial, uint32_t width, uint32_t height) {
    PopupSurface *surf = data;
    bool first_configure = !surf->configured;
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);

    // The desired size was already given explicitly via set_size() at creation,
    // so the compositor's echoed width/height should normally match it exactly
    // -- but honor whatever it actually reports rather than assuming, same as
    // bar_surface.c's own configure handler does.
    if (surf->viewport) {
        // Catcher: the surface's size comes from the viewport destination,
        // not the buffer, so a single transparent pixel is enough -- a full
        // 1920x1080 ARGB buffer is ~8.3 MB per output (16.6 MB with both shm
        // slots mapped), all to hold nothing but zeroes. The destination is
        // double-buffered state, applied by do_render()'s commit below.
        if (width > 0 && height > 0) {
            wp_viewport_set_destination(surf->viewport, (int32_t)width, (int32_t)height);
        }
        if (first_configure && !shm_buffers_resize(&surf->buffers, surf->app->shm, 1, 1)) {
            fprintf(stderr, "stickBar: failed to allocate a 1x1 shm buffer for a catcher surface\n");
            return;
        }
    } else if (first_configure || (int)width != surf->buffers.width || (int)height != surf->buffers.height) {
        if (!shm_buffers_resize(&surf->buffers, surf->app->shm, (int)width, (int)height)) {
            fprintf(stderr, "stickBar: failed to allocate shm buffers for a %ux%u popup surface\n", width, height);
            return;
        }
    }
    surf->configured = true;

    // First-configure self-triggered paint (mirrors bar_surface.c's configure
    // handler calling bar_surface_mark_dirty() to kick off its own first
    // render) -- without this, a popup created and immediately drawn to via
    // popup_surface_render_now() before this event arrives would otherwise
    // never actually paint anything, since that earlier call had nothing to do
    // yet (not configured) and nothing else re-invokes it afterward.
    if (surf->has_pending_draw) {
        do_render(surf);
    }
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *layer_surface) {
    (void)layer_surface;
    // Popups in this project are always torn down explicitly by their owning
    // module (e.g. tray_context_menu.c on outside-click), never expected to
    // receive a compositor-initiated `closed` first -- handled defensively by
    // just marking unconfigured, so a stray render_now() call afterward is a
    // safe no-op instead of touching a surface the compositor already dropped.
    PopupSurface *surf = data;
    surf->configured = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

PopupSurface *popup_surface_create(AppContext *app, Output *output, int width, int height,
                                    int top_margin, int right_margin, int bottom_margin, int left_margin,
                                    uint32_t anchor, enum zwlr_layer_shell_v1_layer layer) {
    PopupSurface *surf = calloc(1, sizeof(PopupSurface));
    if (!surf) {
        perror("stickBar: calloc(PopupSurface)");
        return NULL;
    }
    surf->app = app;

    surf->wl_surface = wl_compositor_create_surface(app->compositor);
    surf->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        app->layer_shell, surf->wl_surface, output->wl_output, layer, "custombar-popup");

    zwlr_layer_surface_v1_set_size(surf->layer_surface, (uint32_t)width, (uint32_t)height);
    zwlr_layer_surface_v1_set_anchor(surf->layer_surface, anchor);
    zwlr_layer_surface_v1_set_margin(surf->layer_surface, top_margin, right_margin, bottom_margin, left_margin);
    // No set_exclusive_zone() call -- a popup shouldn't reserve screen space or
    // push other layer-shell surfaces around the way the bar itself does.
    zwlr_layer_surface_v1_set_keyboard_interactivity(surf->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

    zwlr_layer_surface_v1_add_listener(surf->layer_surface, &layer_surface_listener, surf);

    // Same bootstrap requirement as bar_surface.c: an initial commit with no
    // buffer attached is required before the compositor will send the first
    // `configure` (attaching a buffer any earlier is a protocol violation).
    wl_surface_commit(surf->wl_surface);

    return surf;
}

static void draw_transparent(cairo_t *cr, void *user_data) {
    (void)user_data;
    // CAIRO_OPERATOR_SOURCE writes exactly transparent rather than blending,
    // so the result doesn't depend on the shm buffer's starting contents.
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

PopupSurface *popup_surface_create_catcher(AppContext *app, Output *output) {
    PopupSurface *surf = popup_surface_create(
        app, output, 0, 0, 0, 0, 0, 0,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP);
    if (!surf) {
        return NULL;
    }
    // Safe to attach the viewport now: no configure can have been processed
    // yet (that only happens back in main.c's event loop), so the first
    // configure already sees it and sizes the buffer to 1x1.
    if (app->viewporter) {
        surf->viewport = wp_viewporter_get_viewport(app->viewporter, surf->wl_surface);
    }
    popup_surface_render_now(surf, draw_transparent, NULL);
    return surf;
}

void popup_surface_render_now(PopupSurface *surf, void (*draw)(cairo_t *cr, void *user_data), void *user_data) {
    surf->pending_draw = draw;
    surf->pending_user_data = user_data;
    surf->has_pending_draw = true;

    if (!surf->configured) {
        // Nothing to draw into yet -- layer_surface_configure() will call
        // do_render() itself once the compositor's first configure arrives.
        return;
    }
    do_render(surf);
}

struct wl_surface *popup_surface_wl_surface(PopupSurface *surf) {
    return surf->wl_surface;
}

void popup_surface_destroy(PopupSurface *surf) {
    if (!surf) {
        return;
    }
    shm_buffers_destroy(&surf->buffers);
    if (surf->viewport) {
        wp_viewport_destroy(surf->viewport);
    }
    if (surf->layer_surface) {
        zwlr_layer_surface_v1_destroy(surf->layer_surface);
    }
    if (surf->wl_surface) {
        wl_surface_destroy(surf->wl_surface);
    }
    free(surf);
}
