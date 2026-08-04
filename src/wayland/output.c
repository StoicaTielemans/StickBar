#include "wayland/output.h"

#include <stdio.h>
#include <stdlib.h>

#include "bar/bar_surface.h"

// Highest wl_output version this code understands (adds the `done` and `scale`
// events we depend on). If the compositor advertises less, we bind at whatever it
// offers -- wl_registry_bind() would fail outright if asked for a version the
// server doesn't support.
#define OUTPUT_MAX_VERSION 4

static void handle_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
                             int32_t physical_width, int32_t physical_height, int32_t subpixel,
                             const char *make, const char *model, int32_t transform) {
    // Not needed for this bar (no per-output physical-size-aware layout yet) --
    // present only because wl_output_listener requires every callback to be filled in.
    (void)data; (void)wl_output; (void)x; (void)y; (void)physical_width; (void)physical_height;
    (void)subpixel; (void)make; (void)model; (void)transform;
}

static void handle_mode(void *data, struct wl_output *wl_output, uint32_t flags,
                         int32_t width, int32_t height, int32_t refresh) {
    (void)wl_output; (void)refresh;
    Output *output = data;
    // A wl_output can advertise multiple supported modes; only the one flagged
    // "current" is the one actually in use right now.
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        output->width = width;
        output->height = height;
    }
}

static void handle_scale(void *data, struct wl_output *wl_output, int32_t factor) {
    (void)wl_output;
    Output *output = data;
    // HiDPI/fractional-scale-aware rendering is a known gap in this first pass (see
    // project plan) -- the value isn't even stored, since nothing would read it yet.
    (void)output; (void)factor;
}

static void handle_name(void *data, struct wl_output *wl_output, const char *name) {
    // wl_output version 4+ adds `name`/`description` events (e.g. "DP-2"); not
    // needed since this bar identifies outputs by binding-order index, not name --
    // present only because wl_output_listener requires every callback to be set,
    // and we bind at version 4 to also get `scale` (see OUTPUT_MAX_VERSION).
    (void)data; (void)wl_output; (void)name;
}

static void handle_description(void *data, struct wl_output *wl_output, const char *description) {
    (void)data; (void)wl_output; (void)description;
}

static void handle_done(void *data, struct wl_output *wl_output) {
    (void)wl_output;
    Output *output = data;

    if (output->done_received) {
        // This is a *second* burst of output events (a real monitor
        // reconfiguration, e.g. resolution change) -- live-resizing an
        // already-created bar surface in response isn't implemented in this first
        // pass, so just note it rather than silently doing nothing.
        fprintf(stderr, "customBar: output reconfigured after initial setup "
                         "(live resize not implemented yet, bar geometry may be stale)\n");
        return;
    }

    output->done_received = true;
    output->bar_surface = bar_surface_create(output->app, output);
}

static const struct wl_output_listener output_listener = {
    .geometry = handle_geometry,
    .mode = handle_mode,
    .done = handle_done,
    .scale = handle_scale,
    .name = handle_name,
    .description = handle_description,
};

void output_add(AppContext *app, uint32_t name, uint32_t version) {
    Output *output = calloc(1, sizeof(Output));
    if (!output) {
        perror("customBar: calloc(Output)");
        return;
    }

    uint32_t bind_version = version < OUTPUT_MAX_VERSION ? version : OUTPUT_MAX_VERSION;
    output->wl_output = wl_registry_bind(app->registry, name, &wl_output_interface, bind_version);
    output->app = app;
    output->registry_name = name;
    output->index = app->next_output_index++;

    wl_output_add_listener(output->wl_output, &output_listener, output);

    // Prepend to the list; order doesn't matter for anything except that `index`
    // (assigned above, from binding order) is what actually drives the
    // workspace-range-per-output convention, not list position.
    output->next = app->outputs;
    app->outputs = output;
}

void output_remove(AppContext *app, uint32_t name) {
    Output **cursor = &app->outputs;
    while (*cursor) {
        Output *output = *cursor;
        if (output->registry_name == name) {
            *cursor = output->next;
            if (app->pointer_over == output) {
                app->pointer_over = NULL;
            }
            if (output->bar_surface) {
                bar_surface_destroy(output->bar_surface);
            }
            wl_output_destroy(output->wl_output);
            free(output);
            return;
        }
        cursor = &output->next;
    }
}

void output_mark_all_dirty(AppContext *app) {
    for (Output *output = app->outputs; output; output = output->next) {
        if (output->bar_surface) {
            bar_surface_mark_dirty(output->bar_surface);
        }
    }
}
