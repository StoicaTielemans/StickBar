#include "wayland/event_loop.h"

#include <stdio.h>
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

void wayland_dispatch_prepare(struct wl_display *display) {
    while (wl_display_prepare_read(display) != 0) {
        wl_display_dispatch_pending(display);
    }
    // Flush anything queued by the dispatch_pending calls above (or by earlier
    // request-sending code this iteration) so outgoing requests don't sit in our
    // local buffer indefinitely waiting for someone else to flush them.
    wl_display_flush(display);
}

void wayland_dispatch_read(struct wl_display *display) {
    if (wl_display_read_events(display) == -1) {
        // The connection is broken (compositor died, etc.) -- there's nothing
        // meaningful to recover here; the next wl_display_* call elsewhere will
        // surface the same error and the process should exit.
        return;
    }
    // read_events() only moves raw data into the queue; dispatch_pending() is what
    // actually invokes our listener callbacks for it.
    wl_display_dispatch_pending(display);
}

void wayland_dispatch_cancel(struct wl_display *display) {
    wl_display_cancel_read(display);
}
