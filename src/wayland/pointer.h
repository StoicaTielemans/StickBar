#ifndef CUSTOMBAR_WAYLAND_POINTER_H
#define CUSTOMBAR_WAYLAND_POINTER_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

#include "wayland/context.h"

// wl_seat listener -- install once, right after binding the seat global. Requests a
// wl_pointer when the seat reports pointer capability, and wires up enter/motion/
// button routing (hover tracking + click-to-focus on workspace pills).
extern const struct wl_seat_listener seat_listener;

// The routing behind the wl_pointer listener, split out so the test-control
// channel (testing/test_control.c) can drive exactly the same code without a
// real pointer: each acts on app->pointer_over_surface / pointer_over /
// pointer_x / pointer_y, which the real enter/motion events normally keep
// current. `direction` > 0 is scroll up (louder), as in the axis handler.
void pointer_dispatch_motion(AppContext *app);
void pointer_dispatch_button(AppContext *app, uint32_t button, bool pressed);
void pointer_dispatch_scroll(AppContext *app, int direction);

#endif
