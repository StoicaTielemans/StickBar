//! stickBar: a Wayland status bar for Hyprland, being ported from C to Rust.
//!
//! This file is the C bar's `main.c`: it sets everything up in a deliberate
//! order, runs the event loop, and turns how the loop ended into the exit
//! status. Port progress is tracked in `docs/porting.md`.

// Belt and braces: Cargo.toml already forbids unsafe code crate-wide, but
// stating it here too means the rule is visible to anyone reading the source,
// and it still holds if the file is ever built outside this Cargo.toml.
#![forbid(unsafe_code)]

mod bar;
mod config;
mod hyprland;
mod modules;
mod render;
mod sysmon;
mod testing;
mod util;
mod wayland;

use std::process::ExitCode;

use calloop::{
    EventLoop,
    signals::{Signal, Signals},
};
use smithay_client_toolkit::reexports::{
    calloop_wayland_source::WaylandSource,
    client::{Connection, globals::registry_queue_init},
};

use crate::wayland::App;

/// Entry point. Returning `ExitCode` (instead of calling `process::exit`,
/// which the lint policy blocks) lets every value's destructor run on the way
/// out, so the Wayland connection, surfaces and buffers are always torn down
/// cleanly. That *is* the teardown: there's no separate cleanup code.
///
/// Exit status: 0 after SIGINT/SIGTERM, 1 if setup fails or the compositor
/// connection is lost (a test checks the latter: `test_wayland_disconnect.py`).
fn main() -> ExitCode {
    // 1. Signals FIRST, before anything that could start a thread. A signal
    // source works by blocking the signals and reading them from a file
    // descriptor instead. Blocking only applies to the calling thread and to
    // threads started *after* it, and a thread that doesn't block SIGUSR1 gets
    // the default action, which kills the process. In the C bar, NVML's
    // threads started before the block, so the power-menu keybind killed the
    // bar (BUG.md, "Power menu"). Nothing here starts threads yet, but later
    // modules (audio, D-Bus, NVML) will, so this stays the very first step.
    let signals = match Signals::new(&[Signal::SIGINT, Signal::SIGTERM, Signal::SIGUSR1]) {
        Ok(signals) => signals,
        Err(err) => {
            eprintln!("stickBar: couldn't set up signal handling: {err}");
            return ExitCode::FAILURE;
        }
    };

    // 2. Connect to the compositor (the socket named by $WAYLAND_DISPLAY).
    let conn = match Connection::connect_to_env() {
        Ok(conn) => conn,
        Err(err) => {
            eprintln!(
                "stickBar: failed to connect to the Wayland display ({err}) \
                 (is a Wayland compositor running and WAYLAND_DISPLAY set?)"
            );
            return ExitCode::FAILURE;
        }
    };

    // 3. List the compositor's globals and bind the ones the bar needs. Monitors
    // are reported later, through the event loop, as SCTK learns their details.
    let (globals, mut queue) = match registry_queue_init::<App>(&conn) {
        Ok(pair) => pair,
        Err(err) => {
            eprintln!("stickBar: couldn't read the Wayland globals: {err}");
            return ExitCode::FAILURE;
        }
    };
    let qh = queue.handle();
    let mut app = match App::new(&globals, &qh, testing::enabled()) {
        Ok(app) => app,
        Err(message) => {
            eprintln!("stickBar: {message}");
            return ExitCode::FAILURE;
        }
    };
    // One round trip before the loop starts, as the C bar's wayland_connect()
    // does: the monitors' details arrive and each gets its bar, so everything
    // after this (the test-control channel's first commands) sees them.
    if let Err(err) = queue.roundtrip(&mut app) {
        eprintln!("stickBar: Wayland connection lost during setup ({err}) -- exiting");
        return ExitCode::FAILURE;
    }

    // 4. One event loop, every input a source on it (the C bar's epoll loop):
    // the Wayland socket and the signals for now; timers, Hyprland's socket and
    // the module threads' channels join as modules are ported.
    let mut event_loop: EventLoop<'static, App> = match EventLoop::try_new() {
        Ok(event_loop) => event_loop,
        Err(err) => {
            eprintln!("stickBar: couldn't create the event loop: {err}");
            return ExitCode::FAILURE;
        }
    };
    if let Err(err) = WaylandSource::new(conn, queue).insert(event_loop.handle()) {
        eprintln!("stickBar: couldn't watch the Wayland socket: {}", err.error);
        return ExitCode::FAILURE;
    }
    let signal_source = event_loop
        .handle()
        .insert_source(signals, |event, (), state| {
            if event.signal() == Signal::SIGUSR1 {
                // The power-menu keybind (`pkill -SIGUSR1 -x stickBar`, see README).
                state.toggle_power_menu();
            } else {
                state.exit = true; // SIGINT/SIGTERM: stop after this pass, exit 0
            }
        });
    if let Err(err) = signal_source {
        eprintln!("stickBar: couldn't watch for signals: {}", err.error);
        return ExitCode::FAILURE;
    }

    // The module event sources: timers, Hyprland, the test channel.
    if let Err(err) = start_sources(&event_loop.handle(), &mut app) {
        eprintln!("stickBar: {err}");
        return ExitCode::FAILURE;
    }
    // Hyprland's event socket may drop (e.g. a compositor reload): it gets
    // exactly one reconnect per run, then live updates stay off.
    let mut hyprland_reconnected = false;

    // 5. Run. Each pass: wait for and handle whatever arrived, then draw every
    // bar it made dirty. A dispatch error means the Wayland connection is gone
    // (e.g. the compositor crashed). That must end the loop: a hung-up socket
    // stays "readable" forever, so carrying on would spin at 100% CPU (BUG.md,
    // "Compositor crash turned into a silent 100% CPU spin").
    loop {
        if let Err(err) = event_loop.dispatch(None, &mut app) {
            eprintln!("stickBar: Wayland connection lost ({err}) -- exiting");
            return ExitCode::from(1);
        }
        if app.exit {
            return ExitCode::SUCCESS;
        }
        if std::mem::take(&mut app.hyprland_events_lost) {
            let reconnected =
                !hyprland_reconnected && hyprland::events::start(&event_loop.handle()).is_ok();
            hyprland_reconnected = true;
            if reconnected {
                app.refresh_workspaces(); // catch up on whatever happened meanwhile
            } else {
                eprintln!(
                    "stickBar: Hyprland event socket reconnect failed -- workspace \
                     live-updates are now disabled for this run"
                );
            }
        }
        app.flush_dirty(&qh);
    }
}

/// Adds every module's event source to the loop. An error is fatal (the C
/// bar exited too); a missing Hyprland only disables live workspace updates.
fn start_sources(handle: &calloop::LoopHandle<'static, App>, app: &mut App) -> Result<(), String> {
    // The clock's tick, at the start of every wall-clock second.
    modules::clock::timer::start(handle)?;

    // Hyprland: the workspaces as they are now, then live updates from its
    // event socket. Without the socket the bar still runs; the pills just
    // stop following changes.
    app.refresh_workspaces();
    if let Err(err) = hyprland::events::start(handle) {
        eprintln!(
            "stickBar: {err}\nstickBar: workspace live-updates disabled (couldn't connect to \
             Hyprland's event socket) -- the bar still renders, but workspace pills won't \
             update until it's restarted"
        );
    }

    // CPU/GPU/memory figures, polled every 5 s (the first poll already ran).
    sysmon::timer::start(handle)?;

    // The test-control channel on stdin, only with STICKBAR_TEST_CONTROL=1.
    testing::start(handle);
    Ok(())
}
