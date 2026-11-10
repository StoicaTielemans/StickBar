#include "dbus/dbus_loop.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>

#include "util/timer.h"

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
    return timer_fd_new(CLOCK_MONOTONIC, "dbus timeout");
}

static void rearm_timeout_fd(sd_bus *bus, int timeout_fd) {
    uint64_t deadline_usec;
    if (sd_bus_get_timeout(bus, &deadline_usec) < 0) {
        return; // leave whatever was previously armed; not worth failing over
    }

    if (deadline_usec == UINT64_MAX) {
        timer_fd_set(timeout_fd, 0, 0); // no timeout pending: disarm
        return;
    }
    // sd_bus_get_timeout() returns an *absolute* CLOCK_MONOTONIC deadline, and
    // the timerfd is on CLOCK_MONOTONIC too, so it's armed at that time
    // directly. A deadline already past (sd-bus wants pumping again right
    // away) fires immediately.
    timer_fd_set_abs(timeout_fd, deadline_usec * 1000ULL, 0, 0);
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
