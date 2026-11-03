#include "wayland/event_loop.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

int event_loop_create(void) {
    int fd = epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0) {
        perror("stickBar: epoll_create1");
    }
    return fd;
}

bool event_loop_add_fd(int epoll_fd, int fd, uint32_t events) {
    struct epoll_event ev = { .events = events, .data.fd = fd };
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("stickBar: epoll_ctl(ADD)");
        return false;
    }
    return true;
}

/*
 * Why this dance exists at all:
 *
 * wl_display_dispatch() (the "just block and read" API) isn't safe to call from an
 * external epoll loop, because it *always* performs a blocking read on the display
 * socket if no events are already queued -- there's no way to ask it "only read if
 * data is actually available right now". Mixing that with epoll_wait() on multiple
 * fds (our timerfd, the Hyprland event socket) would mean a `read()` on the Wayland
 * socket could block indefinitely even after epoll_wait() said *some* fd was ready,
 * just not necessarily this one.
 *
 * libwayland's actual solution is the prepare_read/read_events pair:
 *   - wl_display_prepare_read() registers "I intend to read" without reading
 *     anything yet, and fails (returns -1) if there are already-queued events this
 *     thread hasn't dispatched -- in which case those must be drained first, or
 *     prepare_read will just keep failing forever.
 *   - Only *after* epoll_wait() has confirmed the display fd is actually readable is
 *     it safe to call wl_display_read_events(), which performs the real read.
 *   - If epoll_wait() says the display fd is NOT readable, wl_display_cancel_read()
 *     must be called instead to release the "prepare" state -- skipping this leaves
 *     the connection wedged for the next iteration.
 */

// Logs why the connection died. wl_display_get_error() returns the errno libwayland
// recorded when it marked the display as broken (EPIPE/ECONNRESET for a vanished
// compositor, EPROTO for a protocol error it already printed details about).
static void log_connection_lost(struct wl_display *display) {
    int err = wl_display_get_error(display);
    fprintf(stderr, "stickBar: Wayland connection lost (%s) -- exiting\n",
            strerror(err ? err : errno));
}

/*
 * Every failure path below returns false instead of carrying on, and main.c treats
 * that as "stop the loop". This matters more than it looks: once the compositor
 * hangs up, the display fd stays permanently readable (EPOLLHUP), so epoll_wait()
 * returns immediately on every iteration. If a failed read were just ignored, the
 * loop would spin at 100% CPU forever with nothing on screen, instead of exiting.
 * Likewise, wl_display_dispatch_pending() returning -1 inside prepare's retry loop
 * would otherwise never let prepare_read succeed, hanging right here instead.
 */

bool wayland_dispatch_prepare(struct wl_display *display) {
    while (wl_display_prepare_read(display) != 0) {
        if (wl_display_dispatch_pending(display) == -1) {
            log_connection_lost(display);
            return false;
        }
    }
    // EAGAIN just means the socket buffer is full right now; the rest goes out on
    // a later flush. Any other error means the connection is gone.
    if (wl_display_flush(display) == -1 && errno != EAGAIN) {
        wl_display_cancel_read(display);
        log_connection_lost(display);
        return false;
    }
    return true;
}

bool wayland_dispatch_read(struct wl_display *display) {
    if (wl_display_read_events(display) == -1 || wl_display_dispatch_pending(display) == -1) {
        log_connection_lost(display);
        return false;
    }
    return true;
}

void wayland_dispatch_cancel(struct wl_display *display) {
    wl_display_cancel_read(display);
}
