#include "wayland/pointer.h"

#include <linux/input-event-codes.h> // BTN_LEFT, BTN_RIGHT, BTN_MIDDLE

#include "audio/device_popup.h"
#include "audio/mixer_popup.h"
#include "modules/clock/clock.h"
#include "modules/clock/clock_calendar_popup.h"
#include "modules/cpu/cpu.h"
#include "modules/eq/eq.h"
#include "modules/eq/eq_popup.h"
#include "modules/gpu/gpu.h"
#include "modules/media/media.h"
#include "modules/media/media_popup.h"
#include "modules/mem/mem.h"
#include "modules/mic/mic.h"
#include "modules/network/network.h"
#include "modules/network/network_info_popup.h"
#include "modules/powermenu/arch_logo.h"
#include "modules/powermenu/power_menu.h"
#include "modules/privacy/privacy.h"
#include "modules/privacy/privacy_info_popup.h"
#include "modules/tray/tray.h"
#include "modules/tray/tray_context_menu.h"
#include "modules/volume/volume.h"
#include "modules/workspaces/workspaces.h"
#include "modules/cpu/cpu_popup.h"
#include "modules/gpu/gpu_popup.h"
#include "modules/mem/mem_popup.h"
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

// How input on a dropdown's own surfaces is routed (pointer_dispatch_button()
// and pointer_dispatch_motion()). The click handlers differ in what they
// take, so each gets a one-line adapter to one shape. The mixer and the power
// menu aren't in here: they're special-cased where they're handled.
typedef struct {
    bool (*is_menu)(struct wl_surface *surface);
    bool (*is_catcher)(struct wl_surface *surface);
    void (*click)(double x, double y, uint32_t button); // NULL = read-only content
    void (*motion)(double x, double y);                 // NULL = no hover states
    void (*close)(void);
} PopupRoute;

static void tray_menu_click(double x, double y, uint32_t button) {
    (void)button;
    tray_context_menu_handle_click(x, y);
}
static void media_click(double x, double y, uint32_t button) {
    (void)button;
    media_popup_handle_click(x, y);
}
static void eq_click(double x, double y, uint32_t button) {
    (void)button;
    eq_popup_handle_click(x, y);
}

static const PopupRoute POPUP_ROUTES[] = {
    {.is_menu = tray_context_menu_is_menu_surface, .is_catcher = tray_context_menu_is_catcher_surface,
     .click = tray_menu_click, .motion = tray_context_menu_handle_motion, .close = tray_context_menu_close},
    {.is_menu = device_popup_is_menu_surface, .is_catcher = device_popup_is_catcher_surface,
     .click = device_popup_handle_press, .motion = device_popup_handle_motion, .close = device_popup_close},
    {.is_menu = media_popup_is_menu_surface, .is_catcher = media_popup_is_catcher_surface,
     .click = media_click, .motion = media_popup_handle_motion, .close = media_popup_close},
    {.is_menu = eq_popup_is_menu_surface, .is_catcher = eq_popup_is_catcher_surface,
     .click = eq_click, .motion = eq_popup_handle_motion, .close = eq_popup_close},
    // Year navigation in the header; the rest of the calendar is read-only.
    {.is_menu = clock_calendar_popup_is_menu_surface, .is_catcher = clock_calendar_popup_is_catcher_surface,
     .click = clock_calendar_popup_handle_click, .motion = clock_calendar_popup_handle_motion,
     .close = clock_calendar_popup_close},
    {.is_menu = mem_popup_is_menu_surface, .is_catcher = mem_popup_is_catcher_surface,
     .click = NULL, .motion = NULL, .close = mem_popup_close},
    {.is_menu = cpu_popup_is_menu_surface, .is_catcher = cpu_popup_is_catcher_surface,
     .click = NULL, .motion = NULL, .close = cpu_popup_close},
    {.is_menu = gpu_popup_is_menu_surface, .is_catcher = gpu_popup_is_catcher_surface,
     .click = NULL, .motion = NULL, .close = gpu_popup_close},
    {.is_menu = network_info_popup_is_menu_surface, .is_catcher = network_info_popup_is_catcher_surface,
     .click = NULL, .motion = NULL, .close = network_info_popup_close},
    {.is_menu = privacy_info_popup_is_menu_surface, .is_catcher = privacy_info_popup_is_catcher_surface,
     .click = NULL, .motion = NULL, .close = privacy_info_popup_close},
};

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
    if (clock_calendar_popup_is_menu_surface(surface)) {
        clock_calendar_popup_handle_motion(-1.0, -1.0); // drop any header hover highlight
    }
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
    pointer_dispatch_motion(app);
}

void pointer_dispatch_motion(AppContext *app) {
    struct wl_surface *surface = app->pointer_over_surface;
    if (mixer_popup_is_menu_surface(surface)) {
        // Also drives slider drags: Wayland's implicit grab keeps
        // pointer_over_surface on the mixer while the button is held, even
        // if the pointer wanders outside it.
        mixer_popup_handle_motion(app->pointer_x, app->pointer_y);
        return;
    }
    if (power_menu_is_menu_surface(surface)) {
        power_menu_handle_motion(surface, app->pointer_x, app->pointer_y);
        return;
    }
    for (size_t i = 0; i < sizeof(POPUP_ROUTES) / sizeof(POPUP_ROUTES[0]); i++) {
        if (POPUP_ROUTES[i].is_menu(surface)) {
            if (POPUP_ROUTES[i].motion) {
                POPUP_ROUTES[i].motion(app->pointer_x, app->pointer_y);
            }
            return;
        }
    }
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                            uint32_t time, uint32_t button, uint32_t state) {
    (void)pointer; (void)serial; (void)time;
    pointer_dispatch_button(data, button, state == WL_POINTER_BUTTON_STATE_PRESSED);
}

void pointer_dispatch_button(AppContext *app, uint32_t button, bool pressed) {
    if (!pressed) {
        // Releases only matter for ending a volume drag (the mixer's sliders
        // and the device dropdown's meters) -- every other handler here acts
        // on press alone. Unconditional (not gated on which surface it landed
        // on), so a drag always ends even if the release arrives elsewhere.
        mixer_popup_handle_release();
        device_popup_handle_release();
        return;
    }
    if (button != BTN_LEFT && button != BTN_RIGHT && button != BTN_MIDDLE) {
        return;
    }

    // The mixer's own surfaces first, since it's the one popup that cares
    // about which button was pressed (and the only one opened by middle-click).
    if (mixer_popup_is_menu_surface(app->pointer_over_surface)) {
        mixer_popup_handle_press(app->pointer_x, app->pointer_y, button);
        return;
    }
    if (mixer_popup_is_catcher_surface(app->pointer_over_surface)) {
        mixer_popup_close();
        return;
    }
    // Middle-click is only bound on the volume box (opens the mixer). Filtered
    // out here rather than passed down, since most of the handlers below don't
    // take a button at all (popup menu clicks) or treat any non-left button as
    // their secondary action (the tray's has-menu rule), and would otherwise
    // misfire on a middle-click.
    if (button == BTN_MIDDLE) {
        if (app->pointer_over) {
            volume_handle_click(app->pointer_over, app->pointer_x, app->pointer_y, button);
        }
        return;
    }

    // Popup surfaces aren't Output-keyed at all -- a dropdown or catcher
    // surface never matches any Output -- so they're checked first, before the
    // bar-surface path below. The power menu's own full-screen surface doubles
    // as its click-catcher: one call handles a card, the panel and the dimmed
    // background (see power_menu_handle_click()).
    if (power_menu_is_menu_surface(app->pointer_over_surface)) {
        power_menu_handle_click(app->pointer_over_surface, app->pointer_x, app->pointer_y);
        return;
    }
    // Every other dropdown: a click on it goes to its handler (read-only
    // ones just absorb it), a click on one of its catchers closes it. They're
    // independent singletons whose surfaces never overlap, so the order of
    // this table doesn't matter.
    for (size_t i = 0; i < sizeof(POPUP_ROUTES) / sizeof(POPUP_ROUTES[0]); i++) {
        const PopupRoute *r = &POPUP_ROUTES[i];
        if (r->is_menu(app->pointer_over_surface)) {
            if (r->click) {
                r->click(app->pointer_x, app->pointer_y, button);
            }
            return;
        }
        if (r->is_catcher(app->pointer_over_surface)) {
            r->close();
            return;
        }
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
    // all -- see modules/cpu/cpu.h; media's own left-toggles-play/right-
    // opens-dropdown rule, matching volume/mic's shape exactly; network's
    // own right-opens-dropdown-only rule, matching cpu/gpu/mem's shape
    // exactly; privacy's own right-opens-dropdown-only rule, same shape
    // again (and a no-op if it's collapsed, since its recorded rect's w/h
    // are zeroed in that case); arch_logo's own left-click-only rule, checked internally the
    // same way cpu/gpu/mem/network check their own button rules internally
    // rather than being pre-filtered here like workspaces_handle_click()
    // is), so all of them are called unconditionally here regardless of
    // which button fired.
    if (app->pointer_over) {
        if (button == BTN_LEFT) {
            workspaces_handle_click(app->pointer_over, app->pointer_x, app->pointer_y);
        }
        arch_logo_handle_click(app->pointer_over, app->pointer_x, app->pointer_y, button);
        tray_handle_click(app->pointer_over, app->pointer_x, app->pointer_y, button);
        volume_handle_click(app->pointer_over, app->pointer_x, app->pointer_y, button);
        mic_handle_click(app->pointer_over, app->pointer_x, app->pointer_y, button);
        clock_handle_click(app->pointer_over, app->pointer_x, app->pointer_y, button);
        cpu_handle_click(app->pointer_over, app->pointer_x, app->pointer_y, button);
        gpu_handle_click(app->pointer_over, app->pointer_x, app->pointer_y, button);
        mem_handle_click(app->pointer_over, app->pointer_x, app->pointer_y, button);
        privacy_handle_click(app->pointer_over, app->pointer_x, app->pointer_y, button);
        media_handle_click(app->pointer_over, app->pointer_x, app->pointer_y, button);
        network_handle_click(app->pointer_over, app->pointer_x, app->pointer_y, button);
        // Right-click only opens the EQ dropdown -- see modules/eq/eq.h.
        eq_handle_click(app->pointer_over, app->pointer_x, app->pointer_y, button);
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
    pointer_dispatch_scroll(data, discrete < 0 ? 1 : -1); // see comment above
}

void pointer_dispatch_scroll(AppContext *app, int direction) {
    if (mixer_popup_is_menu_surface(app->pointer_over_surface)) {
        mixer_popup_handle_scroll(app->pointer_x, app->pointer_y, direction);
        return;
    }
    if (device_popup_is_menu_surface(app->pointer_over_surface)) {
        device_popup_handle_scroll(app->pointer_x, app->pointer_y, direction);
        return;
    }
    if (!app->pointer_over) {
        return; // scrolled over a popup/catcher surface or nothing at all
    }
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
