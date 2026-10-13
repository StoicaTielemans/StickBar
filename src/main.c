#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "audio/device_popup.h"
#include "audio/mixer_popup.h"
#include "audio/pulse_client.h"
#include "bar/bar_surface.h"
#include "dbus/dbus_loop.h"
#include "dbus/sni_registry.h"
#include "dbus/sni_watcher.h"
#include "dbus/sni_watcher_service.h"
#include "hyprland/ipc_events.h"
#include "modules/clock/clock.h"
#include "modules/media/media_popup.h"
#include "modules/powermenu/power_menu.h"
#include "modules/workspaces/workspaces.h"
#include "mpris/mpris_client.h"
#include "network/network_client.h"
#include "pipewire/pipewire_client.h"
#include "sysmon/sysmon_client.h"
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
        perror("stickBar: timerfd_create");
        return -1;
    }
    struct itimerspec spec = {
        .it_interval = { .tv_sec = 1, .tv_nsec = 0 },
        .it_value = { .tv_sec = 1, .tv_nsec = 0 },
    };
    if (timerfd_settime(fd, 0, &spec, NULL) < 0) {
        perror("stickBar: timerfd_settime");
        close(fd);
        return -1;
    }
    return fd;
}

// 5-second interval, matching the reference SystemMonitor.qml's own polling
// Timer exactly -- also drives network_client_poll() below (see that call
// site), since NetworkMonitor.qml's own Timer uses the identical 5000ms
// interval and reusing this one avoids a second timerfd for the same cadence.
static int create_sysmon_timer(void) {
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (fd < 0) {
        perror("stickBar: timerfd_create(sysmon)");
        return -1;
    }
    struct itimerspec spec = {
        .it_interval = { .tv_sec = 5, .tv_nsec = 0 },
        .it_value = { .tv_sec = 5, .tv_nsec = 0 },
    };
    if (timerfd_settime(fd, 0, &spec, NULL) < 0) {
        perror("stickBar: timerfd_settime(sysmon)");
        close(fd);
        return -1;
    }
    return fd;
}

// Delivers SIGUSR1 through a signalfd (registered on the same epoll loop as
// everything else) rather than a classic signal() handler -- this is the
// Hyprland-keybind trigger for the power menu (`pkill -SIGUSR1 -x stickBar`
// or equivalent; see CLAUDE.md's "Power menu" section for the exact keybind
// line), and unlike g_should_exit's flip-a-flag-and-let-the-main-loop-notice
// approach, signalfd lets this be handled as just another readable fd with
// no async-signal-safety concerns at all, since nothing runs until the main
// loop's own read() of it. SIGUSR1 must be blocked via sigprocmask() first --
// signalfd only ever reports signals that are also blocked from their normal
// asynchronous delivery path; otherwise it would either be delivered async
// (unsafe to act on directly) or, since SIGUSR1's default disposition is to
// terminate the process, kill the bar the first time the keybind is used.
// Called early in main(), before pulse_client_init() spawns its own
// background thread -- POSIX signal masks are per-thread but inherited at
// creation time, so blocking SIGUSR1 here ensures that thread inherits the
// same blocked mask too (the correct pattern for signalfd in a
// multi-threaded process: block everywhere, only the thread holding the
// signalfd itself ever actually consumes a pending instance).
static int create_powermenu_signal_fd(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        perror("stickBar: sigprocmask(SIGUSR1)");
        return -1;
    }
    int fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (fd < 0) {
        perror("stickBar: signalfd(SIGUSR1)");
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
    // Stashes `app` for power_menu_toggle() to use when opening -- see
    // power_menu_init()'s own doc comment for why it needs this (the SIGUSR1
    // trigger path below has no Output/AppContext of its own to hand over).
    power_menu_init(&app);

    // Populate an initial Hyprland snapshot, clock string, and sysmon snapshot
    // before the first frame renders, then mark every already-bound output
    // dirty so that first frame actually happens instead of waiting on the
    // first tick/event. sysmon_client_init() only establishes the /proc/stat
    // baseline; the actual first poll (which will show 0% CPU, no prior
    // sample to diff against yet -- see sysmon_client.h) happens right after,
    // matching the reference SystemMonitor.qml's own `triggeredOnStart: true`.
    workspaces_refresh();
    clock_tick();
    sysmon_client_init();
    sysmon_client_poll();
    // No network_client_init() equivalent needed -- see network_client.h's
    // own doc comment for why this module has no delta-based baseline to
    // seed beyond what its first poll below already self-primes.
    network_client_poll();
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

    int sysmon_timer_fd = create_sysmon_timer();
    if (sysmon_timer_fd < 0 || !event_loop_add_fd(epoll_fd, sysmon_timer_fd, EPOLLIN)) {
        return 1;
    }

    // Power menu (Hyprland keybind trigger): an optional module, same
    // "degrade gracefully" convention as every other fd registered below --
    // if signalfd() itself fails for some reason, the bar still runs fine,
    // just without a SIGUSR1 trigger (the Arch logo button still works
    // regardless, since it doesn't go through this fd at all).
    int powermenu_signal_fd = create_powermenu_signal_fd();
    if (powermenu_signal_fd >= 0 && !event_loop_add_fd(epoll_fd, powermenu_signal_fd, EPOLLIN)) {
        close(powermenu_signal_fd);
        powermenu_signal_fd = -1;
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
        fprintf(stderr, "stickBar: workspace live-updates disabled (couldn't connect to "
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
            // Try to become the StatusNotifierWatcher ourselves first (so the
            // tray keeps working even with no other Watcher-providing app
            // running at all); only fall back to connecting as a Host client
            // if something else already owns the well-known bus name.
            if (!sni_watcher_service_try_start(bus)) {
                sni_watcher_register_host(bus);
            }
            // Media (MPRIS): reuses this same session-bus connection rather
            // than opening a second one -- see mpris/mpris_client.h's own
            // doc comment. No separate "degrade gracefully" branch needed
            // here beyond the tray's own: mpris_client_init() just finds
            // zero players if nothing's playing, the same as an empty tray.
            mpris_client_init(bus, &app);
            // Flushes the registration/subscription requests just made above and
            // re-derives dbus_fd's real interest mask/timeout, same as every
            // other dbus_loop_pump() call later in the main loop.
            dbus_loop_pump(bus, epoll_fd, dbus_fd, dbus_timeout_fd);
        }
    }

    // Volume/mic: an optional module, same "degrade gracefully" convention as
    // the tray's D-Bus connection above -- a missing/broken PulseAudio-compatible
    // server just means the volume/mic boxes render "--" and stay
    // non-interactive for this run, not a fatal error.
    bool pulse_ok = pulse_client_init();
    int pulse_fd = -1;
    if (pulse_ok) {
        pulse_fd = pulse_client_notify_fd();
        if (!event_loop_add_fd(epoll_fd, pulse_fd, EPOLLIN)) {
            pulse_client_shutdown();
            pulse_ok = false;
            pulse_fd = -1;
        }
    }

    // Privacy indicator (mic/webcam/screen-share): an optional module, same
    // "degrade gracefully" convention as the tray's D-Bus connection and
    // pulse_client above -- a missing/broken PipeWire connection just means
    // the privacy module stays permanently collapsed for this run, not a
    // fatal error. Placed after pulse_client_init() (order between the two
    // doesn't matter -- no cross-dependency) but still after
    // create_powermenu_signal_fd()'s sigprocmask() call above, for the same
    // reason pulse_client_init() must be: pw_thread_loop_start() spawns a
    // background thread, which inherits the calling thread's signal mask at
    // creation time.
    bool pipewire_ok = pipewire_client_init();
    int pipewire_fd = -1;
    if (pipewire_ok) {
        pipewire_fd = pipewire_client_notify_fd();
        if (!event_loop_add_fd(epoll_fd, pipewire_fd, EPOLLIN)) {
            pipewire_client_shutdown();
            pipewire_ok = false;
            pipewire_fd = -1;
        }
    }

    struct epoll_event events[10];

    while (!g_should_exit) {
        // Must be called immediately before epoll_wait() on every iteration -- see
        // wayland/event_loop.c for why a plain wl_display_dispatch() can't be used
        // here instead.
        wayland_dispatch_prepare(app.display);

        int n = epoll_wait(epoll_fd, events, 10, -1);
        if (n < 0) {
            if (errno == EINTR) {
                wayland_dispatch_cancel(app.display);
                continue;
            }
            perror("stickBar: epoll_wait");
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
                // Piggybacks on this already-ticking 1s timer rather than
                // adding a new poll source -- see media_popup_tick()'s own
                // doc comment for why MPRIS position needs polling at all.
                // A no-op unless the media popup is actually open.
                media_popup_tick();
                output_mark_all_dirty(&app);
            } else if (fd == sysmon_timer_fd) {
                uint64_t expirations;
                (void)!read(sysmon_timer_fd, &expirations, sizeof(expirations));
                // Includes a synchronous nvidia-smi subprocess call -- see
                // sysmon_client_poll()'s own doc comment and CLAUDE.md's
                // "System monitor (CPU/GPU/memory)" section for why that's an
                // acceptable blocking window here (bounded, ~15-45ms, once
                // per 5 seconds, not per-frame or per-click).
                sysmon_client_poll();
                // Piggybacks on this same 5-second timer rather than adding
                // a second poll source -- matches the reference
                // NetworkMonitor.qml's own identical 5000ms interval.
                network_client_poll();
                output_mark_all_dirty(&app);
            } else if (powermenu_signal_fd >= 0 && fd == powermenu_signal_fd) {
                struct signalfd_siginfo info;
                // Drains every pending instance (there's realistically ever
                // only one, since SIGUSR1 isn't queued/repeated the way a
                // real-time signal would be) -- required regardless, or the
                // fd stays permanently "readable" the same way a timerfd
                // does if its own expiration count isn't read.
                while (read(powermenu_signal_fd, &info, sizeof(info)) == sizeof(info)) {
                }
                power_menu_toggle();
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
                // Cheap no-op unless the media popup is actually open (see
                // media_popup_refresh_if_open()'s own doc comment). The bar
                // widget itself doesn't need an equivalent unconditional
                // output_mark_all_dirty() call here -- unlike pulse_client's
                // notify-fd below (only ever signaled on an actual cache
                // change), this fd is ready on *any* D-Bus activity, most of
                // which doesn't touch tray or MPRIS state, so each module's
                // own signal handler (sni_registry.c's, mpris_client.c's)
                // already calls output_mark_all_dirty() itself, granularly,
                // only when something actually changed.
                media_popup_refresh_if_open();
            } else if (pulse_ok && fd == pulse_fd) {
                // The cache itself is already updated by the time this fires
                // (pulse_client's background thread writes to it before
                // signaling) -- pump() just drains the eventfd counter.
                pulse_client_pump();
                device_popup_refresh_if_open();
                mixer_popup_refresh_if_open();
                output_mark_all_dirty(&app);
            } else if (pipewire_ok && fd == pipewire_fd) {
                // Same shape as the pulse_fd branch above: this fd is only
                // ever signaled on an actual node/link graph change, so an
                // unconditional redraw here (rather than each module calling
                // it granularly, the dbus_fd branch's approach) is correct.
                pipewire_client_pump();
                output_mark_all_dirty(&app);
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
                fprintf(stderr, "stickBar: Hyprland event socket reconnect failed -- "
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
    if (pulse_ok) {
        pulse_client_shutdown();
    }
    if (pipewire_ok) {
        pipewire_client_shutdown();
    }
    if (dbus_timeout_fd >= 0) {
        close(dbus_timeout_fd);
    }
    close(timer_fd);
    close(sysmon_timer_fd);
    if (powermenu_signal_fd >= 0) {
        close(powermenu_signal_fd);
    }
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
