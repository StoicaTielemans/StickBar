#include "wayland/registry.h"

#include <stdio.h>
#include <string.h>

#include "wayland/output.h"
#include "wayland/pointer.h"

// Versions requested when binding each global. Picked to be the highest version
// this project's code actually knows how to use, not necessarily the compositor's
// max -- wl_registry_bind() will fail if we ask for a version the compositor
// doesn't support, so these should only be bumped alongside code that needs the
// newer behavior.
#define COMPOSITOR_VERSION 4
#define SHM_VERSION 1
#define SEAT_VERSION 7
#define LAYER_SHELL_VERSION 4

static void handle_global(void *data, struct wl_registry *registry, uint32_t name,
                           const char *interface, uint32_t version) {
    AppContext *app = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        app->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, COMPOSITOR_VERSION);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        app->shm = wl_registry_bind(registry, name, &wl_shm_interface, SHM_VERSION);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        app->seat = wl_registry_bind(registry, name, &wl_seat_interface, SEAT_VERSION);
        wl_seat_add_listener(app->seat, &seat_listener, app);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        app->layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, LAYER_SHELL_VERSION);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        output_add(app, name, version);
    }
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)registry;
    AppContext *app = data;
    output_remove(app, name);
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

bool wayland_connect(AppContext *app) {
    app->display = wl_display_connect(NULL);
    if (!app->display) {
        fprintf(stderr, "gridBar: failed to connect to the Wayland display "
                         "(is a Wayland compositor running and WAYLAND_DISPLAY set?)\n");
        return false;
    }

    app->registry = wl_display_get_registry(app->display);
    wl_registry_add_listener(app->registry, &registry_listener, app);

    // First roundtrip: the server replies to our get_registry with a burst of
    // `global` events for everything currently advertised, which our listener
    // processes synchronously -- binding compositor/shm/seat/layer_shell and, for
    // each wl_output, sending its own bind request and attaching its listener.
    wl_display_roundtrip(app->display);

    // Second roundtrip: the bind requests we just sent (in particular for each
    // wl_output) only got flushed to the server as part of *this* roundtrip, so
    // their own event bursts (geometry/mode/scale/done) haven't been delivered yet
    // after the first one returns. This second roundtrip lets those arrive and get
    // dispatched, so every already-present output has done_received=true (and thus
    // a real bar surface) by the time this function returns.
    wl_display_roundtrip(app->display);

    if (!app->compositor || !app->shm || !app->layer_shell) {
        fprintf(stderr, "gridBar: compositor is missing a required Wayland global "
                         "(wl_compositor=%p wl_shm=%p zwlr_layer_shell_v1=%p) -- "
                         "is this running under a wlroots-based compositor?\n",
                (void *)app->compositor, (void *)app->shm, (void *)app->layer_shell);
        return false;
    }

    return true;
}
