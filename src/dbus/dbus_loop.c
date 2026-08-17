#include "dbus/dbus_loop.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>

sd_bus *dbus_loop_connect(void) {
    sd_bus *bus = NULL;
    int rc = sd_bus_open_user(&bus);
    if (rc < 0) {
        fprintf(stderr, "stickBar: failed to connect to the D-Bus session bus (%s) "
                         "-- system tray disabled for this run\n", strerror(-rc));
        return NULL;
    }
    return bus;
}

int dbus_loop_fd(sd_bus *bus) {
    return sd_bus_get_fd(bus);
}

int dbus_loop_create_timeout_fd(void) {
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) {
        perror("stickBar: timerfd_create(dbus timeout)");
    }
    return fd;
}

static void rearm_timeout_fd(sd_bus *bus, int timeout_fd) {
    uint64_t deadline_usec;
    if (sd_bus_get_timeout(bus, &deadline_usec) < 0) {
        return; // leave whatever was previously armed; not worth failing over
    }

    struct itimerspec spec = { 0 };
    if (deadline_usec != UINT64_MAX) {
        // sd_bus_get_timeout() returns an *absolute* CLOCK_MONOTONIC deadline in
        // microseconds; timerfd_settime() wants a *relative* itimerspec, so this
        // converts between the two. A deadline already in the past (sd-bus wants
        // to be pumped again immediately) clamps to the smallest positive
        // interval instead of zero -- an all-zero itimerspec.it_value would
        // *disarm* the timer instead of firing it immediately, the opposite of
        // what's needed here.
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t now_usec = (uint64_t)now.tv_sec * 1000000ULL + (uint64_t)now.tv_nsec / 1000ULL;
        uint64_t remaining_usec = deadline_usec > now_usec ? deadline_usec - now_usec : 1;

        spec.it_value.tv_sec = (time_t)(remaining_usec / 1000000ULL);
        spec.it_value.tv_nsec = (long)((remaining_usec % 1000000ULL) * 1000ULL);
    }
    // deadline_usec == UINT64_MAX means "no timeout pending" -- spec stays
    // all-zero, which disarms the timer.

    timerfd_settime(timeout_fd, 0, &spec, NULL);
}

void dbus_loop_pump(sd_bus *bus, int epoll_fd, int dbus_fd, int timeout_fd) {
    // sd_bus_process() performs one unit of work (dispatch one incoming message,
    // flush one queued outgoing send, etc.) and returns 0 once there's nothing
    // left to do without blocking -- looping until it returns 0 is sd-bus's own
    // documented pattern for draining everything currently available in one pass.
    int rc;
    do {
        rc = sd_bus_process(bus, NULL);
    } while (rc > 0);

    // sd-bus's interest in the fd changes over time (e.g. it wants EPOLLOUT while
    // a large outgoing call is still being flushed), so this must be re-applied
    // after every pump rather than set once at connect time.
    int events = sd_bus_get_events(bus);
    if (events >= 0) {
        struct epoll_event ev = { .events = (uint32_t)events, .data.fd = dbus_fd };
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, dbus_fd, &ev);
    }

    rearm_timeout_fd(bus, timeout_fd);
}

void dbus_loop_disconnect(sd_bus *bus) {
    if (bus) {
        sd_bus_flush_close_unref(bus);
    }
}
