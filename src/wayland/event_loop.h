#ifndef CUSTOMBAR_WAYLAND_EVENT_LOOP_H
#define CUSTOMBAR_WAYLAND_EVENT_LOOP_H

#include <stdbool.h>
#include <wayland-client.h>

// Creates an epoll instance. Returns -1 on failure.
int event_loop_create(void);

// Registers `fd` on `epoll_fd` for the given event mask (e.g. EPOLLIN). Returns
// false on failure.
bool event_loop_add_fd(int epoll_fd, int fd, uint32_t events);

// These three functions implement libwayland's "prepare_read" protocol for safely
// mixing a Wayland display fd into an external epoll loop -- see event_loop.c for
// why a plain wl_display_dispatch() can't be used here. Call wayland_dispatch_prepare()
// immediately before every epoll_wait(), then call exactly one of
// wayland_dispatch_read() (if the display fd came back readable) or
// wayland_dispatch_cancel() (if it didn't) before the next loop iteration.
void wayland_dispatch_prepare(struct wl_display *display);
void wayland_dispatch_read(struct wl_display *display);
void wayland_dispatch_cancel(struct wl_display *display);

#endif
