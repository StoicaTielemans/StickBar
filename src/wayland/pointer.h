#ifndef CUSTOMBAR_WAYLAND_POINTER_H
#define CUSTOMBAR_WAYLAND_POINTER_H

#include <wayland-client.h>

// wl_seat listener -- install once, right after binding the seat global. Requests a
// wl_pointer when the seat reports pointer capability, and wires up enter/motion/
// button routing (hover tracking + click-to-focus on workspace pills).
extern const struct wl_seat_listener seat_listener;

#endif
