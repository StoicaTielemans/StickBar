#include "wayland/pointer.h"

#include <linux/input-event-codes.h> // BTN_LEFT, BTN_RIGHT

#include "audio/device_popup.h"
#include "modules/clock/clock.h"
#include "modules/clock/clock_calendar_popup.h"
#include "modules/cpu/cpu.h"
#include "modules/gpu/gpu.h"
#include "modules/mem/mem.h"
#include "modules/mic/mic.h"
#include "modules/tray/tray.h"
#include "modules/tray/tray_context_menu.h"
#include "modules/volume/volume.h"
#include "modules/workspaces/workspaces.h"
#include "sysmon/sysmon_info_popup.h"
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
    } else if (device_popup_is_menu_surface(app->pointer_over_surface)) {
        device_popup_handle_motion(app->pointer_x, app->pointer_y);
    }
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                            uint32_t time, uint32_t button, uint32_t state) {
    (void)pointer; (void)serial; (void)time;
    AppContext *app = data;
    if (state != WL_POINTER_BUTTON_STATE_PRESSED || (button != BTN_LEFT && button != BTN_RIGHT)) {
        return;
    }

    // Popup-surface clicks (menu entries + click-catcher, for both the tray's
    // context menu and the volume/mic device dropdown) aren't Output-keyed at
    // all -- checked first, before falling through to the bar-surface path
    // below, since a popup/catcher surface never matches any Output. The two
    // popup kinds are independent singletons (see audio/device_popup.h) --
    // having both open at once isn't defended against here, just checked for
    // in whichever order happens to be written below.
    if (tray_context_menu_is_menu_surface(app->pointer_over_surface)) {
        tray_context_menu_handle_click(app->pointer_x, app->pointer_y);
        return;
    }
    if (tray_context_menu_is_catcher_surface(app->pointer_over_surface)) {
        tray_context_menu_close();
        return;
    }
    if (device_popup_is_menu_surface(app->pointer_over_surface)) {
        device_popup_handle_click(app->pointer_x, app->pointer_y);
        return;
    }
    if (device_popup_is_catcher_surface(app->pointer_over_surface)) {
        device_popup_close();
        return;
    }
    if (sysmon_info_popup_is_menu_surface(app->pointer_over_surface)) {
        // Read-only content -- a click lands here but there's nothing to
        // dispatch to, unlike the two interactive popups above; absorbed so
        // it doesn't fall through to the bar-surface path below (which would
        // be wrong anyway, since this surface never matches any Output).
        return;
    }
    if (sysmon_info_popup_is_catcher_surface(app->pointer_over_surface)) {
        sysmon_info_popup_close();
        return;
    }
    if (clock_calendar_popup_is_menu_surface(app->pointer_over_surface)) {
        // Read-only content, same as the sysmon info popup above.
        return;
    }
    if (clock_calendar_popup_is_catcher_surface(app->pointer_over_surface)) {
        clock_calendar_popup_close();
        return;
    }

    // Bar-surface clicks (tray icons, workspace pills, volume/mic/cpu/gpu/mem
    // boxes): explicit call sites rather than a generic dispatch table, same
    // reasoning as before -- each handler already does its own hit-testing
    // internally, so this is a flat list of "ask everyone, whoever's rect
    // matches wins," not an indirection layer worth building at this N.
    // workspaces_handle_click() only ever reacts to left-click (its own
    // internal convention, unrelated to the others); every other handler
    // branches on `button` internally instead (the tray's has-menu-overrides-
    // button rule; volume/mic's left-mutes/right-opens-dropdown rule;
    // cpu/gpu/mem's right-opens-dropdown-only rule, no left-click action at
    // all -- see modules/cpu/cpu.h), so all of them are called unconditionally
    // here regardless of which button fired.
    if (app->pointer_over) {
        if (button == BTN_LEFT) {
            workspaces_handle_click(app->pointer_over, app->pointer_x, app->pointer_y);
        }
        tray_handle_click(app->pointer_over, app->pointer_x, app->pointer_y, button);
        volume_handle_click(app->pointer_over, app->pointer_x, app->pointer_y, button);
        mic_handle_click(app->pointer_over, app->pointer_x, app->pointer_y, button);
        clock_handle_click(app->pointer_over, app->pointer_x, app->pointer_y, button);
        cpu_handle_click(app->pointer_over, app->pointer_x, app->pointer_y, button);
        gpu_handle_click(app->pointer_over, app->pointer_x, app->pointer_y, button);
        mem_handle_click(app->pointer_over, app->pointer_x, app->pointer_y, button);
    }
}

// Axis/scroll input is otherwise unused by any bar module -- these remain
// no-op stubs present only because wl_pointer_listener requires every callback
// in the struct to be non-NULL. pointer_axis_discrete() below is the one
// exception, driving the volume/mic modules' scroll-to-adjust.
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

// Drives the volume/mic modules' "scroll to adjust by one step" behavior.
// `discrete` is ±1 (occasionally more on some drivers, but magnitude is
// deliberately ignored -- one call here is meant to represent exactly one
// wheel notch, matching the "steps of N%" behavior volume_handle_scroll()/
// mic_handle_scroll() implement) per full wheel click; SEAT_VERSION=7
// (wayland/registry.c) is what makes this event (added in wl_pointer v5)
// available at all -- the newer, higher-resolution axis_value120 (v8) isn't,
// since the pointer object is only ever created at the bound seat's version.
//
// Direction convention: per wayland.xml, the axis `value`/`discrete` sign
// shares "a coordinate space identical to those of motion events" (y
// increasing downward) -- so a positive value corresponds to the same
// direction as scrolling a page *down*. This maps the conventional way most
// Linux volume-control scroll bindings do: scrolling up (`discrete < 0`)
// increases, scrolling down (`discrete > 0`) decreases. Not independently
// verified against this machine's actual mouse (no working synthetic-input
// setup here to test with -- see the tray's own honestly-flagged margin-math
// fix for the same caveat pattern); trivial to flip if it turns out backwards
// in practice.
static void pointer_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t discrete) {
    (void)p;
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL || discrete == 0) {
        return;
    }
    AppContext *app = data;
    if (!app->pointer_over) {
        return; // scrolled over a popup/catcher surface or nothing at all
    }
    int direction = discrete < 0 ? 1 : -1; // see comment above
    volume_handle_scroll(app->pointer_over, app->pointer_x, app->pointer_y, direction);
    mic_handle_scroll(app->pointer_over, app->pointer_x, app->pointer_y, direction);
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
