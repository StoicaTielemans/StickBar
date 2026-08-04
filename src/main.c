#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "bar/bar_surface.h"
#include "dbus/dbus_loop.h"
#include "dbus/sni_registry.h"
#include "dbus/sni_watcher.h"
#include "hyprland/ipc_events.h"
#include "modules/clock/clock.h"
#include "modules/workspaces/workspaces.h"
#include "wayland/context.h"
#include "wayland/event_loop.h"
#include "wayland/output.h"
#include "wayland/registry.h"

// SIGINT/SIGTERM just flip this flag; the actual teardown happens once the current
// epoll_wait() call returns, so every resource is released cleanly through the
// normal code path rather than an async-signal-unsafe cleanup running inside the
// handler itself.
static volatile sig_atomic_t g_should_exit = 0;

static void handle_signal(int sig) {
    (void)sig;
    g_should_exit = 1;
}

static int create_clock_timer(void) {
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (fd < 0) {
        perror("customBar: timerfd_create");
        return -1;
    }
    struct itimerspec spec = {
        .it_interval = { .tv_sec = 1, .tv_nsec = 0 },
        .it_value = { .tv_sec = 1, .tv_nsec = 0 },
    };
    if (timerfd_settime(fd, 0, &spec, NULL) < 0) {
        perror("customBar: timerfd_settime");
        close(fd);
        return -1;
    }
    return fd;
}

// Drops the current Hyprland event connection (if any) and tries once to
// reconnect, re-registering the new fd on epoll. This is the "one reconnect
// attempt, then disable the module" policy from the project plan -- it lives here
// in main.c (which owns epoll registration) rather than inside hyprland/ipc_events.c,
// so the retry policy stays in one place instead of being split across two files.
static HyprlandEvents *reconnect_hypr_events(int epoll_fd, HyprlandEvents *old, int old_fd) {
    if (old) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, old_fd, NULL);
        hyprland_events_destroy(old);
    }

    HyprlandEvents *ev = hyprland_events_connect();
    if (!ev) {
        return NULL;
    }
    if (!event_loop_add_fd(epoll_fd, hyprland_events_fd(ev), EPOLLIN)) {
        hyprland_events_destroy(ev);
        return NULL;
    }
    return ev;
}

int main(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    AppContext app = {0};
    if (!wayland_connect(&app)) {
        return 1;
    }

    // Populate an initial Hyprland snapshot and clock string before the first
    // frame renders, then mark every already-bound output dirty so that first
    // frame actually happens instead of waiting on the first tick/event.
    workspaces_refresh();
    clock_tick();
    output_mark_all_dirty(&app);

    int epoll_fd = event_loop_create();
    if (epoll_fd < 0) {
        return 1;
    }

    int wayland_fd = wl_display_get_fd(app.display);
    if (!event_loop_add_fd(epoll_fd, wayland_fd, EPOLLIN)) {
        return 1;
    }

    int timer_fd = create_clock_timer();
    if (timer_fd < 0 || !event_loop_add_fd(epoll_fd, timer_fd, EPOLLIN)) {
        return 1;
    }

    HyprlandEvents *hypr_events = hyprland_events_connect();
    int hypr_fd = -1;
    if (hypr_events) {
        hypr_fd = hyprland_events_fd(hypr_events);
        if (!event_loop_add_fd(epoll_fd, hypr_fd, EPOLLIN)) {
            hyprland_events_destroy(hypr_events);
            hypr_events = NULL;
            hypr_fd = -1;
        }
    }
    if (!hypr_events) {
        fprintf(stderr, "customBar: workspace live-updates disabled (couldn't connect to "
                         "Hyprland's event socket) -- the bar still renders, but workspace "
                         "pills won't update until it's restarted\n");
    }

    // System tray: an optional module, same "degrade gracefully" convention as
    // the Hyprland event socket above -- a missing/broken D-Bus session bus or
    // StatusNotifierWatcher just means the tray stays empty, not a fatal error.
    sd_bus *bus = dbus_loop_connect();
    int dbus_fd = -1, dbus_timeout_fd = -1;
    if (bus) {
        dbus_fd = dbus_loop_fd(bus);
        dbus_timeout_fd = dbus_loop_create_timeout_fd();
        if (dbus_timeout_fd < 0 || !event_loop_add_fd(epoll_fd, dbus_fd, EPOLLIN) ||
            !event_loop_add_fd(epoll_fd, dbus_timeout_fd, EPOLLIN)) {
            dbus_loop_disconnect(bus);
            bus = NULL;
            dbus_fd = dbus_timeout_fd = -1;
        } else {
            sni_registry_init(&app);
            sni_watcher_register_host(bus);
            // Flushes the registration/subscription requests just made above and
            // re-derives dbus_fd's real interest mask/timeout, same as every
            // other dbus_loop_pump() call later in the main loop.
            dbus_loop_pump(bus, epoll_fd, dbus_fd, dbus_timeout_fd);
        }
    }

    struct epoll_event events[8];

    while (!g_should_exit) {
        // Must be called immediately before epoll_wait() on every iteration -- see
        // wayland/event_loop.c for why a plain wl_display_dispatch() can't be used
        // here instead.
        wayland_dispatch_prepare(app.display);

        int n = epoll_wait(epoll_fd, events, 8, -1);
        if (n < 0) {
            if (errno == EINTR) {
                wayland_dispatch_cancel(app.display);
                continue;
            }
            perror("customBar: epoll_wait");
            wayland_dispatch_cancel(app.display);
            break;
        }

        bool wayland_ready = false;
        bool hypr_broke = false;

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == wayland_fd) {
                wayland_ready = true;
            } else if (fd == timer_fd) {
                uint64_t expirations;
                // Value is unused (we redraw once per wakeup regardless of how many
                // 1s intervals elapsed) -- reading it is only to drain the timerfd,
                // which is required or it stays permanently "readable".
                (void)!read(timer_fd, &expirations, sizeof(expirations));
                clock_tick();
                output_mark_all_dirty(&app);
            } else if (hypr_events && fd == hypr_fd) {
                if (!hyprland_events_pump(hypr_events, &app)) {
                    hypr_broke = true;
                }
            } else if (bus && (fd == dbus_fd || fd == dbus_timeout_fd)) {
                if (fd == dbus_timeout_fd) {
                    uint64_t expirations;
                    (void)!read(dbus_timeout_fd, &expirations, sizeof(expirations));
                }
                dbus_loop_pump(bus, epoll_fd, dbus_fd, dbus_timeout_fd);
            }
        }

        if (wayland_ready) {
            wayland_dispatch_read(app.display);
        } else {
            wayland_dispatch_cancel(app.display);
        }

        if (hypr_broke) {
            hypr_events = reconnect_hypr_events(epoll_fd, hypr_events, hypr_fd);
            hypr_fd = hypr_events ? hyprland_events_fd(hypr_events) : -1;
            if (!hypr_events) {
                fprintf(stderr, "customBar: Hyprland event socket reconnect failed -- "
                                 "workspace live-updates are now disabled for this run\n");
            }
        }
    }

    if (hypr_events) {
        hyprland_events_destroy(hypr_events);
    }
    if (bus) {
        dbus_loop_disconnect(bus);
    }
    if (dbus_timeout_fd >= 0) {
        close(dbus_timeout_fd);
    }
    close(timer_fd);
    close(epoll_fd);

    // Explicitly tear down every output's bar surface (releasing its layer-shell
    // exclusive zone) rather than just disconnecting -- relying on the compositor
    // to clean up after a dropped connection isn't guaranteed to happen promptly,
    // and the verification checklist specifically checks `hyprctl monitors -j`'s
    // `reserved` field goes back to normal after exit.
    while (app.outputs) {
        Output *output = app.outputs;
        app.outputs = output->next;
        if (output->bar_surface) {
            bar_surface_destroy(output->bar_surface);
        }
        wl_output_destroy(output->wl_output);
        free(output);
    }

    if (app.pointer) {
        wl_pointer_release(app.pointer);
    }
    wl_display_disconnect(app.display);

    return 0;
}
