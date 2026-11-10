#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "audio/device_popup.h"
#include "audio/mixer_popup.h"
#include "audio/pulse_client.h"
#include "bar/bar_surface.h"
#include "dbus/dbus_loop.h"
#include "dbus/sni_registry.h"
#include "dbus/sni_watcher.h"
#include "dbus/sni_watcher_service.h"
#include "eq/eq_state.h"
#include "hyprland/ipc_events.h"
#include "modules/clock/clock.h"
#include "modules/eq/eq_popup.h"
#include "modules/media/media_popup.h"
#include "modules/powermenu/power_menu.h"
#include "modules/workspaces/workspaces.h"
#include "mpris/mpris_client.h"
#include "network/network_client.h"
#include "modules/cpu/cpu_popup.h"
#include "modules/gpu/gpu_popup.h"
#include "modules/mem/mem_popup.h"
#include "modules/network/network_info_popup.h"
#include "pipewire/pipewire_client.h"
#include "visualizer/visualizer.h"
#include "sysmon/sysmon_client.h"
#include "testing/test_control.h"
#include "util/timer.h"
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

/*
 * The clock timer fires at the start of every wall-clock second, not every
 * second since startup. A plain CLOCK_MONOTONIC 1s interval (what this used to
 * be) ticks at whatever sub-second offset the bar happened to start at, so the
 * displayed seconds could lag the real time by up to a full second -- visible
 * as two bars started at different moments showing different seconds.
 *
 *   - CLOCK_REALTIME + TFD_TIMER_ABSTIME, first expiry at the next whole second:
 *     an absolute periodic timer keeps every later expiry on .000 too, and a
 *     timerfd never fires early, so localtime() at expiry always sees the new
 *     second.
 *   - TFD_TIMER_CANCEL_ON_SET: if the wall clock is *set* (NTP step, manual
 *     change, resume from suspend), the next read() fails with ECANCELED instead
 *     of the timer silently staying on the old alignment -- main loop then calls
 *     arm_clock_timer() again to re-align to the new time.
 */
#define NS_PER_S 1000000000ULL

static bool arm_clock_timer(int fd) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return timer_fd_set_abs(fd, ((uint64_t)now.tv_sec + 1) * NS_PER_S, NS_PER_S, TFD_TIMER_CANCEL_ON_SET);
}

static int create_clock_timer(void) {
    int fd = timer_fd_new(CLOCK_REALTIME, "clock");
    if (fd < 0) {
        return -1;
    }
    if (!arm_clock_timer(fd)) {
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
    int fd = timer_fd_new(CLOCK_MONOTONIC, "sysmon");
    if (fd >= 0 && !timer_fd_set(fd, 5 * NS_PER_S, 5 * NS_PER_S)) {
        close(fd);
        return -1;
    }
    return fd;
}

// Delivers SIGUSR1 through a signalfd (registered on the same epoll loop as
// everything else) rather than a classic signal() handler -- this is the
// Hyprland-keybind trigger for the power menu (`pkill -SIGUSR1 -x stickBar`
// or equivalent; see README.md's "Power menu keybind" for the exact keybind
// line), and unlike g_should_exit's flip-a-flag-and-let-the-main-loop-notice
// approach, signalfd lets this be handled as just another readable fd with
// no async-signal-safety concerns at all, since nothing runs until the main
// loop's own read() of it. SIGUSR1 must be blocked via sigprocmask() first --
// signalfd only ever reports signals that are also blocked from their normal
// asynchronous delivery path; otherwise it would either be delivered async
// (unsafe to act on directly) or, since SIGUSR1's default disposition is to
// terminate the process, kill the bar the first time the keybind is used.
//
// The blocking and the signalfd are two steps on purpose. POSIX signal masks
// are per-thread, inherited when a thread is created, and a process-directed
// signal (pkill) goes to *any* thread that doesn't block it -- so SIGUSR1 has
// to be blocked before the first thread exists, i.e. as the very first thing
// in main() (block_powermenu_signal()). It used to be blocked here, after
// sysmon_client_init(): fine while the GPU came from nvidia-smi, but NVML's
// init starts two threads of its own, which then had SIGUSR1 unblocked -- the
// kernel delivered the keybind's signal to one of them and its default
// action killed the bar. tests/test_power_menu.py guards this.
static bool block_powermenu_signal(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        perror("stickBar: sigprocmask(SIGUSR1)");
        return false;
    }
    return true;
}

static int create_powermenu_signal_fd(bool blocked) {
    if (!blocked) {
        return -1; // unblocked, signalfd would never see it -- and SIGUSR1 stays fatal
    }
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    int fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (fd < 0) {
        perror("stickBar: signalfd(SIGUSR1)");
    }
    return fd;
}

// Starts an optional module -- one whose failure just disables it for this
// run ("degrade gracefully", as with every source below) -- and registers
// its fd on epoll. Returns the fd, or -1 if the module didn't start or its fd
// couldn't be registered (in which case it's shut down again).
static int start_optional(int epoll_fd, bool (*init)(void), int (*get_fd)(void), void (*shutdown)(void)) {
    if (!init()) {
        return -1;
    }
    int fd = get_fd();
    if (fd < 0 || !event_loop_add_fd(epoll_fd, fd, EPOLLIN)) {
        shutdown();
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
    // First, before anything can start a thread (NVML, PulseAudio and
    // PipeWire all do) -- see create_powermenu_signal_fd()'s comment.
    bool sigusr1_blocked = block_powermenu_signal();
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
    int powermenu_signal_fd = create_powermenu_signal_fd(sigusr1_blocked);
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
    int pulse_fd = start_optional(epoll_fd, pulse_client_init, pulse_client_notify_fd, pulse_client_shutdown);

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
    int pipewire_fd = start_optional(epoll_fd, pipewire_client_init, pipewire_client_notify_fd,
                                     pipewire_client_shutdown);

    // Media visualizer (the spectrum graphs in the media dropdown): optional,
    // same "degrade gracefully" convention -- without it the dropdown just has
    // no graphs. Its thread starts on the first capture, but its init still
    // comes after the sigprocmask() at the top of main() like the others. Its
    // timerfd is only armed while the dropdown is open and something plays.
    int visualizer_fd = start_optional(epoll_fd, visualizer_init, visualizer_timer_fd, visualizer_shutdown);

    // DAC EQ (TRN Black Pearl): an optional module, same "degrade
    // gracefully" convention as everything above -- if the inotify watch
    // can't be set up, the EQ box still shows whatever state was loaded at
    // startup, it just won't live-update for this run. Needs no thread, so
    // its position relative to the sigprocmask() call above doesn't matter.
    int eq_fd = start_optional(epoll_fd, eq_state_init, eq_state_watch_fd, eq_state_shutdown);

    // Test-only: STICKBAR_TEST_CONTROL=1 makes stdin a command channel for
    // tests/ (see testing/test_control.h). -1 -- and stdin never touched --
    // in normal use.
    int test_fd = test_control_init(&app);
    if (test_fd >= 0 && !event_loop_add_fd(epoll_fd, test_fd, EPOLLIN)) {
        test_fd = -1;
    }

    struct epoll_event events[10];

    // Set when the compositor connection dies, so main() exits non-zero after the
    // normal teardown below instead of pretending this was a clean shutdown.
    bool wayland_lost = false;

    while (!g_should_exit) {
        // Must be called immediately before epoll_wait() on every iteration -- see
        // wayland/event_loop.c for why a plain wl_display_dispatch() can't be used
        // here instead. A false return means the connection is already dead.
        if (!wayland_dispatch_prepare(app.display)) {
            wayland_lost = true;
            break;
        }

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
                // One redraw per wakeup, however many seconds elapsed.
                if (!timer_fd_drain(timer_fd) && errno == ECANCELED) {
                    // The wall clock was set -- re-align to the new second
                    // boundary (see arm_clock_timer()). Still tick below: the
                    // time being shown just changed.
                    arm_clock_timer(timer_fd);
                }
                clock_tick();
                // Piggybacks on this already-ticking 1s timer rather than
                // adding a new poll source -- see media_popup_tick()'s own
                // doc comment for why MPRIS position needs polling at all.
                // A no-op unless the media popup is actually open.
                media_popup_tick();
                // Fills the CPU dropdown's process list ~1s after it opens.
                cpu_popup_tick();
                // Only the clock changed -- repaint just its box, not the whole
                // bar (see bar_surface_mark_clock_dirty()).
                output_mark_all_clock_dirty(&app);
            } else if (fd == sysmon_timer_fd) {
                timer_fd_drain(sysmon_timer_fd);
                // All in-process reads (/proc, /sys, NVML) -- no subprocess.
                sysmon_client_poll();
                // Moves the GPU dropdown's graphs along; no-op if closed.
                gpu_popup_refresh_if_open();
                cpu_popup_refresh_if_open();
                mem_popup_refresh_if_open();
                // Piggybacks on this same 5-second timer rather than adding
                // a second poll source -- matches the reference
                // NetworkMonitor.qml's own identical 5000ms interval.
                network_client_poll();
                // Moves the network dropdown's graph along; no-op if closed.
                network_info_popup_refresh_if_open();
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
                    timer_fd_drain(dbus_timeout_fd);
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
            } else if (pulse_fd >= 0 && fd == pulse_fd) {
                // The cache itself is already updated by the time this fires
                // (pulse_client's background thread writes to it before
                // signaling) -- pump() just drains the eventfd counter.
                pulse_client_pump();
                device_popup_refresh_if_open();
                mixer_popup_refresh_if_open();
                output_mark_all_dirty(&app);
            } else if (pipewire_fd >= 0 && fd == pipewire_fd) {
                // Same shape as the pulse_fd branch above: this fd is only
                // ever signaled on an actual node/link graph change, so an
                // unconditional redraw here (rather than each module calling
                // it granularly, the dbus_fd branch's approach) is correct.
                pipewire_client_pump();
                // Players' playback streams may have come or gone.
                media_popup_streams_changed();
                output_mark_all_dirty(&app);
            } else if (visualizer_fd >= 0 && fd == visualizer_fd) {
                // Each graph lives on its own small subsurface in the media
                // dropdown, so a frame never touches the popup's own pixels.
                // A graph that can't show a new frame right now (the
                // compositor hasn't presented its last one) isn't analysed
                // this tick either.
                uint32_t shown = 0, ready = 0;
                media_popup_visualizer_status(&shown, &ready);
                uint32_t changed = visualizer_pump(ready, shown);
                if (changed) {
                    media_popup_visualizer_frame(changed);
                }
            } else if (eq_fd >= 0 && fd == eq_fd) {
                // Fires on any write in ~/.cache/blackpearl-eq/; pump() only
                // reports a change when state.json itself was (re)written and
                // parsed, so the redraw stays granular like the dbus branch.
                if (eq_state_pump()) {
                    eq_popup_refresh_if_open();
                    output_mark_all_dirty(&app);
                }
            } else if (test_fd >= 0 && fd == test_fd) {
                if (!test_control_pump()) {
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, test_fd, NULL);
                    test_fd = -1;
                }
                output_mark_all_dirty(&app);
            }
        }

        if (wayland_ready) {
            if (!wayland_dispatch_read(app.display)) {
                wayland_lost = true;
                break;
            }
        } else {
            wayland_dispatch_cancel(app.display);
        }

        // The visualizer only captures for media-dropdown cards whose
        // player is playing, while the dropdown is open -- closed, it costs
        // nothing (streams paused, timer disarmed). That can change from
        // several places (MPRIS signals on the D-Bus fd, clicks, players
        // coming and going), so this syncs once per iteration; a no-op per
        // slot unless its answer changed.
        if (visualizer_fd >= 0) {
            media_popup_sync_visualizer();
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
    if (pulse_fd >= 0) {
        pulse_client_shutdown();
    }
    if (pipewire_fd >= 0) {
        pipewire_client_shutdown();
    }
    if (visualizer_fd >= 0) {
        visualizer_shutdown();
    }
    if (eq_fd >= 0) {
        eq_state_shutdown();
    }
    sysmon_client_shutdown();
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
        output_remove(&app, app.outputs->registry_name);
    }

    if (app.pointer) {
        wl_pointer_release(app.pointer);
    }
    wl_display_disconnect(app.display);

    return wayland_lost ? 1 : 0;
}
