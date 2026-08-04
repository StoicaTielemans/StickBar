#include "wayland/pointer.h"

#include <linux/input-event-codes.h> // BTN_LEFT, BTN_RIGHT

#include "modules/tray/tray.h"
#include "modules/tray/tray_context_menu.h"
#include "modules/workspaces/workspaces.h"
#include "wayland/context.h"

// Finds the Output whose bar surface owns `surface`, or NULL if none does (e.g. the
// pointer entered some other client's surface, which shouldn't normally reach us,
// but is handled defensively).
static Output *output_for_surface(AppContext *app, struct wl_surface *surface) {
    for (Output *output = app->outputs; output; output = output->next) {
        if (output->wl_surface == surface) {
            return output;
        }
    }
    return NULL;
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                           struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
    (void)pointer; (void)serial;
    AppContext *app = data;
    app->pointer_over_surface = surface;
    // NULL for a popup/click-catcher surface -- those are never stored on any
    // Output (see context.h) -- which is fine, since only the bar-surface click
    // path below actually needs it.
    app->pointer_over = output_for_surface(app, surface);
    app->pointer_x = wl_fixed_to_double(sx);
    app->pointer_y = wl_fixed_to_double(sy);
    // No cursor image is set here (see meson.build's note on wayland-cursor) --
    // clicks still work, the pointer glyph just doesn't change on hover yet.
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                           struct wl_surface *surface) {
    (void)pointer; (void)serial;
    AppContext *app = data;
    if (app->pointer_over_surface == surface) {
        app->pointer_over_surface = NULL;
        app->pointer_over = NULL;
    }
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                            wl_fixed_t sx, wl_fixed_t sy) {
    (void)pointer; (void)time;
    AppContext *app = data;
    app->pointer_x = wl_fixed_to_double(sx);
    app->pointer_y = wl_fixed_to_double(sy);

    if (tray_context_menu_is_menu_surface(app->pointer_over_surface)) {
        tray_context_menu_handle_motion(app->pointer_x, app->pointer_y);
    }
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                            uint32_t time, uint32_t button, uint32_t state) {
    (void)pointer; (void)serial; (void)time;
    AppContext *app = data;
    if (state != WL_POINTER_BUTTON_STATE_PRESSED || (button != BTN_LEFT && button != BTN_RIGHT)) {
        return;
    }

    // Popup-surface clicks (menu entries + click-catcher) aren't Output-keyed at
    // all -- checked first, before falling through to the bar-surface path
    // below, since a popup/catcher surface never matches any Output.
    if (tray_context_menu_is_menu_surface(app->pointer_over_surface)) {
        tray_context_menu_handle_click(app->pointer_x, app->pointer_y);
        return;
    }
    if (tray_context_menu_is_catcher_surface(app->pointer_over_surface)) {
        tray_context_menu_close();
        return;
    }

    // Bar-surface clicks (tray icons + workspace pills): two explicit call
    // sites rather than a generic dispatch table -- with the tray as the
    // second clickable module, N=2 doesn't yet justify that indirection.
    // Revisit if/when a third bar-surface-clickable module lands (volume/
    // network are on the roadmap).
    if (app->pointer_over) {
        if (button == BTN_LEFT) {
            workspaces_handle_click(app->pointer_over, app->pointer_x, app->pointer_y);
        }
        tray_handle_click(app->pointer_over, app->pointer_x, app->pointer_y, button);
    }
}

// Scroll/axis input isn't used by any bar module in this pass -- these are no-op
// stubs present only because wl_pointer_listener requires every callback in the
// struct to be non-NULL, not because we expect them to fire meaningfully.
static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value) {
    (void)data; (void)p; (void)time; (void)axis; (void)value;
}
static void pointer_frame(void *data, struct wl_pointer *p) {
    (void)data; (void)p;
}
static void pointer_axis_source(void *data, struct wl_pointer *p, uint32_t axis_source) {
    (void)data; (void)p; (void)axis_source;
}
static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis) {
    (void)data; (void)p; (void)time; (void)axis;
}
static void pointer_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t discrete) {
    (void)data; (void)p; (void)axis; (void)discrete;
}
static void pointer_axis_value120(void *data, struct wl_pointer *p, uint32_t axis, int32_t value120) {
    (void)data; (void)p; (void)axis; (void)value120;
}
static void pointer_axis_relative_direction(void *data, struct wl_pointer *p, uint32_t axis, uint32_t direction) {
    (void)data; (void)p; (void)axis; (void)direction;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
    .axis_value120 = pointer_axis_value120,
    .axis_relative_direction = pointer_axis_relative_direction,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
    AppContext *app = data;
    bool has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;

    if (has_pointer && !app->pointer) {
        app->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(app->pointer, &pointer_listener, app);
    } else if (!has_pointer && app->pointer) {
        wl_pointer_release(app->pointer);
        app->pointer = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    // Not needed -- we don't distinguish between multiple seats.
    (void)data; (void)seat; (void)name;
}

const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};
