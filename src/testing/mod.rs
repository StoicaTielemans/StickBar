//! The test-control channel (the C bar's `testing/test_control.c`).
//!
//! With `STICKBAR_TEST_CONTROL=1`, stdin becomes a command channel for the
//! Python tests (`tests/lib/common.py`'s `ControlledBar`): one command per line
//! in, one JSON reply per line on stdout. Tests use it to read where module
//! boxes are, inject clicks without moving the user's real cursor, and dump
//! what data sources hold. Without the variable, stdin is never read.
//!
//! Command names, arguments and reply shapes match the C bar exactly, so the
//! same tests run against both. Commands are added as their modules are ported:
//!
//! ```text
//! ping                   -> {"ok":true}
//! outputs                -> {"ok":true,"outputs":[{name,index,width,height,has_bar}]}
//! rects <name|index>     -> {"ok":true,"rects":{module:[x,y,w,h],...}}
//! click <output> <x> <y> <left|right|middle>
//!                        -> a click on that monitor's bar, bar-local px
//! menu                   -> {"ok":true,"open":bool,"width","height","catchers",
//!                            "power_last_action","device_cards","mixer_rows","eq_cards",
//!                            "media_cards","tray_entries" (only while a tray menu is open)}
//! menu-click <x> <y> [button] / menu-motion <x> <y>
//!                        -> a click / move on the open menu, menu-local px
//! menu-press <x> <y> [button] / menu-release / menu-scroll <x> <y> <+1|-1>
//!                        -> press (a drag follows with menu-motion), release,
//!                           one wheel step
//! catcher-click          -> a click on a click-catcher (closes the dropdown)
//! snapshot <png>         -> the open menu's picture, drawn again into a PNG
//! target power <logout|restart|shutdown|corner|header>
//! target device <meter|mute|body> <fraction> <description...>
//! target mixer <slider|mute> <fraction> <stream name...>
//! target eq <profile>
//! target traymenu <label...> / target trayicon <output> <title...>
//! target media <prev|next|play|timeline:FRACTION> <app name...>
//!                        -> {"ok":true,"x","y"}: a point on the power menu, a
//!                           device or mixer card, or an EQ profile card
//! refresh <mem|cpu|cpu-reset|gpu|poll>
//!                        -> run a dropdown's details refresh (cpu-reset: start
//!                           a fresh process window first), or a full 5 s poll
//! dump <mem|cpu|gpu|net|audio|eq|media>
//!                        -> every figure the bar holds (test_sysmon_data,
//!                           test_network_data, test_audio, test_eq)
//! cursor <output> <x> <y> / menu-cursor <x> <y>
//!                        -> {"ok":true,"shape":"pointer"|"default"}: the cursor
//!                           the bar would show there (Rust only)
//! anything else          -> {"ok":false,"error":"unknown command"}
//! ```
//!
//! Injected input goes through `App::pointer_button`/`pointer_motion`, the
//! same routing real pointer events take, so only the compositor's "which
//! surface is under the cursor" step is skipped. Nothing on screen is clicked
//! and the user's cursor never moves.

use std::{
    fs::File,
    io::{self, Read, Write},
    os::fd::AsFd,
};

use calloop::{
    Interest, LoopHandle, Mode, PostAction,
    generic::{Generic, NoIoDrop},
};
use serde_json::{Value, json};
use smithay_client_toolkit::reexports::protocols::wp::cursor_shape::v1::client::wp_cursor_shape_device_v1::Shape;

use crate::{
    audio::{AudioState, Kind, Node},
    bar::{BarSurface, popup_surface::PopupSurface},
    dbus::Toggle,
    eq::EqState,
    modules::media::Media,
    network::Network,
    render::{Action, ClickTarget},
    sysmon::Sysmon,
    wayland::{App, BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, Output},
};

/// The environment variable that turns the channel on (exactly `1`, as in C).
const ENABLE_VAR: &str = "STICKBAR_TEST_CONTROL";

/// How much is read from stdin per wake-up. A command is a few dozen bytes;
/// anything longer than this just arrives over several wake-ups.
const READ_CHUNK: usize = 4096;

/// Whether this instance is being driven by the tests.
pub(crate) fn enabled() -> bool {
    std::env::var(ENABLE_VAR).is_ok_and(|value| value == "1")
}

/// Starts watching stdin for commands, if the channel is enabled.
///
/// stdin's file descriptor is duplicated into a plain `File` so every read is a
/// direct `read()` syscall. `io::stdin()` keeps its own buffer, and data sitting
/// in that buffer wouldn't make the descriptor readable, so a command could
/// wait unnoticed. fd 0 itself stays open and untouched.
pub(crate) fn start(handle: &LoopHandle<'static, App>) {
    if !enabled() {
        return;
    }
    let file = match io::stdin().as_fd().try_clone_to_owned() {
        Ok(fd) => File::from(fd),
        Err(err) => {
            eprintln!("stickBar: test-control: couldn't open stdin: {err}");
            return;
        }
    };
    // Level-triggered: as long as unread data is waiting, the loop keeps waking
    // this source, so one read per wake-up is enough and stdin never needs to
    // be switched to non-blocking mode (a read only happens when data is ready).
    let source = Generic::new(file, Interest::READ, Mode::Level);
    let mut pending = Vec::new();
    let inserted = handle.insert_source(source, move |_readiness, stdin, app| {
        Ok(pump(stdin, &mut pending, app))
    });
    match inserted {
        Ok(_token) => eprintln!("stickBar: test-control channel enabled on stdin"),
        Err(err) => eprintln!(
            "stickBar: test-control: couldn't watch stdin: {}",
            err.error
        ),
    }
}

/// Reads what's available, runs every complete line, and tells the loop
/// whether to keep watching.
fn pump(file: &NoIoDrop<File>, pending: &mut Vec<u8>, app: &mut App) -> PostAction {
    let mut chunk = [0_u8; READ_CHUNK];
    // `Read` is implemented for `&File`, which is what the loop hands out.
    let read = (&**file).read(&mut chunk);
    let keep_watching = match read {
        Ok(count) if count > 0 => {
            pending.extend(chunk.iter().take(count));
            true
        }
        // EOF (the test closed its end) or an error: stop watching. A closed
        // pipe stays "readable" forever, so keeping the source would wake the
        // loop endlessly at 100% CPU. Lines already received still run below.
        _ => false,
    };

    while let Some(newline) = pending.iter().position(|&byte| byte == b'\n') {
        let line: Vec<u8> = pending.drain(..=newline).collect();
        let line = String::from_utf8_lossy(&line);
        let line = line.trim();
        if !line.is_empty() {
            let reply = dispatch(app, line);
            // Commands can change what's shown (later: open a popup, inject a
            // click), so every bar redraws after one, as in the C bar. Bars
            // whose pixels didn't change skip the commit anyway.
            app.mark_all_dirty();
            send(&reply);
        }
    }

    if keep_watching {
        PostAction::Continue
    } else {
        PostAction::Remove
    }
}

/// Runs one command and builds its reply.
fn dispatch(app: &mut App, line: &str) -> Value {
    let (command, rest) = line.split_once(' ').unwrap_or((line, ""));
    let rest = rest.trim();
    match command {
        "ping" => json!({ "ok": true }),
        "outputs" => outputs(app),
        "rects" if !rest.is_empty() => rects(app, rest),
        "click" if !rest.is_empty() => click(app, rest),
        "menu" => menu(app),
        "menu-click" | "menu-motion" | "menu-press" | "menu-scroll" if !rest.is_empty() => {
            menu_input(app, command, rest)
        }
        "menu-release" => {
            app.pointer_release();
            json!({ "ok": true })
        }
        "catcher-click" => catcher_click(app),
        "cursor" if !rest.is_empty() => cursor(app, rest),
        "target" if !rest.is_empty() => target(app, rest),
        "refresh" if !rest.is_empty() => refresh(app, rest),
        "dump" if !rest.is_empty() => dump(app, rest),
        "menu-cursor" if !rest.is_empty() => menu_cursor(app, rest),
        "snapshot" if !rest.is_empty() => match app.snapshot(rest) {
            Ok(()) => json!({ "ok": true }),
            Err(err) => error(&err),
        },
        _ => error("unknown command"),
    }
}

/// `click <output> <x> <y> <button>`: a press on that monitor's bar.
fn click(app: &mut App, args: &str) -> Value {
    let usage = || error("usage: click <output> <x> <y> <button>");
    let mut parts = args.split_whitespace();
    let (Some(name), Some(x), Some(y), Some(button)) =
        (parts.next(), parts.next(), parts.next(), parts.next())
    else {
        return usage();
    };
    let (Ok(x), Ok(y)) = (x.parse::<f64>(), y.parse::<f64>()) else {
        return usage();
    };
    // A copy of the surface handle (cheap: a reference count), so `app` is free
    // to be borrowed mutably by the click itself.
    let surface = find_output(app, name)
        .and_then(|o| o.bar.as_ref())
        .map(|bar| bar.wl_surface().clone());
    let Some(surface) = surface else {
        return usage();
    };
    app.pointer_button(&surface, x, y, parse_button(button));
    json!({ "ok": true })
}

/// `menu`: whether a dropdown is open, its size, and how many catchers exist.
fn menu(app: &App) -> Value {
    let catchers = app.popups().catchers().count();
    let last = app.popups().power.last_action();
    // -1 when the device dropdown is closed, as in C.
    let device_cards = app
        .popups()
        .device
        .card_count()
        .map_or(-1, |n| i64::try_from(n).unwrap_or(i64::MAX));
    let mixer_rows = app
        .popups()
        .mixer
        .row_count()
        .map_or(-1, |n| i64::try_from(n).unwrap_or(i64::MAX));
    // While a tray menu is open: every entry as shown, separators included.
    let tray_entries = app.popups().tray.entries().map(|entries| {
        entries
            .iter()
            .map(|e| {
                json!({
                    "label": e.label,
                    "separator": e.separator,
                    "enabled": e.enabled,
                    "toggle": match e.toggle.map(|t| t.kind) {
                        Some(Toggle::Checkmark) => "checkmark",
                        Some(Toggle::Radio) => "radio",
                        None => "",
                    },
                    "toggled": e.toggle.is_some_and(|t| t.on),
                    "submenu": e.submenu,
                })
            })
            .collect::<Vec<_>>()
    });
    let media_cards = app
        .popups()
        .media
        .card_count()
        .map_or(-1, |n| i64::try_from(n).unwrap_or(i64::MAX));
    let eq_cards = app
        .popups()
        .eq
        .card_count()
        .map_or(-1, |n| i64::try_from(n).unwrap_or(i64::MAX));
    let mut reply = match app.popups().latest_menu().and_then(PopupSurface::menu_size) {
        Some((width, height)) => json!({
            "ok": true, "open": true, "width": width, "height": height,
            "catchers": catchers, "power_last_action": last, "device_cards": device_cards,
            "mixer_rows": mixer_rows, "eq_cards": eq_cards, "media_cards": media_cards,
        }),
        None => json!({
            "ok": true, "open": false, "catchers": catchers, "power_last_action": last,
            "device_cards": device_cards, "mixer_rows": mixer_rows, "eq_cards": eq_cards,
            "media_cards": media_cards,
        }),
    };
    // Only present while a tray menu is open, as in C.
    if let (Some(entries), Some(object)) = (tray_entries, reply.as_object_mut()) {
        let _previous = object.insert("tray_entries".to_owned(), Value::from(entries));
    }
    reply
}

/// `refresh <what>`: what an open dropdown or the 5 s timer would do, now.
fn refresh(app: &mut App, what: &str) -> Value {
    let sysmon = &mut app.renderer_mut().sysmon;
    match what {
        "mem" => sysmon.mem.refresh_details(),
        "cpu" => sysmon.cpu.refresh_details(),
        "gpu" => sysmon.gpu.refresh_details(),
        "cpu-reset" => {
            sysmon.cpu.reset_process_sample();
            sysmon.cpu.refresh_details();
        }
        "poll" => {
            // A full 5 s poll, plus the network details an open dropdown would read.
            sysmon.poll();
            let network = &mut app.renderer_mut().network;
            network.poll();
            network.refresh_details();
        }
        _ => return error("unknown refresh target"),
    }
    json!({ "ok": true })
}

/// `dump <section>`: what a data source holds, with the C bar's field names.
fn dump(app: &mut App, section: &str) -> Value {
    let renderer = app.renderer_mut();
    match section {
        "mem" => dump_mem(&renderer.sysmon),
        "cpu" => dump_cpu(&renderer.sysmon),
        "gpu" => dump_gpu(&renderer.sysmon),
        "net" => dump_net(&renderer.network),
        "audio" => dump_audio(&renderer.audio),
        "eq" => dump_eq(&renderer.eq),
        "media" => dump_media(&renderer.media),
        _ => error("unknown dump section"),
    }
}

/// `dump mem` (read by `test_sysmon_data`).
fn dump_mem(sysmon: &Sysmon) -> Value {
    let m = &sysmon.mem;
    let zram = m.zram.clone().unwrap_or_default();
    let procs: Vec<Value> = m
        .procs
        .iter()
        .map(|(name, gb)| json!({ "name": name, "gb": gb }))
        .collect();
    json!({
        "ok": true,
        "percent": m.percent,
        "total_gb": m.total_gb,
        "apps_gb": m.used_gb,
        "avail_gb": m.avail_gb,
        "free_gb": m.free_gb,
        "cache_gb": m.cache_gb(),
        "shared_gb": m.shared_gb,
        "dirty_mb": m.dirty_mb,
        "swap_used_gb": m.swap_used_gb,
        "swap_total_gb": m.swap_total_gb,
        "swap_percent": m.swap_percent,
        "history_len": sysmon.mem_history.len(),
        "psi": m.psi.is_some(),
        "psi_some": m.psi.unwrap_or_default(),
        "zram": m.zram.is_some(),
        "zram_algorithm": zram.algorithm,
        "zram_orig_mb": zram.orig_mb,
        "zram_compr_mb": zram.compr_mb,
        "zram_used_mb": zram.used_mb,
        "procs": procs,
    })
}

/// `dump cpu` (read by `test_sysmon_data`).
fn dump_cpu(sysmon: &Sysmon) -> Value {
    let c = &sysmon.cpu;
    let procs: Vec<Value> = c
        .procs
        .iter()
        .map(|(name, percent)| json!({ "name": name, "percent": percent }))
        .collect();
    let efficient: Vec<bool> = (0..c.cores.len()).map(|i| c.is_efficient(i)).collect();
    json!({
        "ok": true,
        "model": c.model,
        "physical_cores": c.physical_cores,
        "threads": c.cores.len(),
        "hybrid": c.hybrid,
        "max_ghz": c.max_ghz,
        "percent": c.percent,
        "temp_c": c.temp_c,
        "load": c.load,
        "tasks_running": c.tasks_running,
        "tasks_total": c.tasks_total,
        "uptime_s": c.uptime_s,
        "history_len": sysmon.cpu_history.len(),
        "efficient": efficient,
        "core_ghz": c.core_ghz,
        "procs_ready": c.procs_ready,
        "procs": procs,
    })
}

/// `dump gpu`.
fn dump_gpu(sysmon: &Sysmon) -> Value {
    let g = &sysmon.gpu;
    let procs: Vec<Value> = g
        .procs
        .iter()
        .map(|(name, mib)| json!({ "name": name, "mib": mib }))
        .collect();
    json!({
        "ok": true,
        "available": g.available,
        "name": g.name,
        "driver": g.driver,
        "percent": g.percent,
        "temp_c": g.temp_c,
        "mem_used_mb": g.mem_used_mb,
        "mem_total_mb": g.mem_total_mb,
        "power_w": g.power_w,
        "power_limit_w": g.power_limit_w,
        "pstate": g.pstate.map(|p| format!("P{p}")).unwrap_or_default(),
        "history_len": sysmon.gpu_history.len(),
        "procs": procs,
    })
}

/// `dump audio` (read by `test_audio`): every sink, source and app stream.
fn dump_audio(audio: &AudioState) -> Value {
    let nodes = |kind: Kind, nodes: &[Node]| -> Vec<Value> {
        let default = audio.default_node(kind).map(|n| n.index);
        nodes
            .iter()
            .map(|n| {
                json!({
                    "description": n.description,
                    "volume": n.volume,
                    "muted": n.muted,
                    "default": default == Some(n.index),
                    "state": n.state,
                    "bus": n.bus,
                    "rate": n.rate,
                    "channels": n.channels,
                    "monitor": n.monitor,
                })
            })
            .collect()
    };
    let streams: Vec<Value> = audio
        .streams
        .iter()
        .map(|s| json!({ "name": s.name, "volume": s.volume, "muted": s.muted }))
        .collect();
    json!({
        "ok": true,
        "sinks": nodes(Kind::Sink, &audio.sinks),
        "sources": nodes(Kind::Source, &audio.sources),
        "streams": streams,
    })
}

/// `dump media` (read by `test_media`): the visible players.
fn dump_media(media: &Media) -> Value {
    let players: Vec<Value> = media
        .players
        .iter()
        .map(|p| {
            json!({
                "app": p.app, "title": p.title, "artist": p.artist, "album": p.album,
                "status": p.status, "length_us": p.length_us,
            })
        })
        .collect();
    json!({ "ok": true, "players": players })
}

/// `dump eq` (read by `test_eq`). `pending` is "" when nothing is, as in C.
fn dump_eq(eq: &EqState) -> Value {
    let snap = &eq.snap;
    json!({
        "ok": true,
        "available": snap.available,
        "profile": snap.profile,
        "gain": snap.gain,
        "preamp": snap.preamp,
        "enabled": snap.enabled,
        "pending": eq.pending.as_deref().unwrap_or_default(),
        "curve_points": snap.curve.len(),
    })
}

/// `dump net` (read by `test_network_data`).
fn dump_net(n: &Network) -> Value {
    json!({
        "ok": true,
        "iface": n.iface,
        "connected": n.connected,
        "wifi": n.wifi,
        "ip": n.ip,
        "gateway": n.gateway,
        "mac": n.mac,
        "mtu": n.mtu,
        "speed": n.speed,
        "rx_rate": n.rx_rate,
        "tx_rate": n.tx_rate,
        "rx_total": n.rx_total,
        "tx_total": n.tx_total,
        "history_len": n.history.len(),
        "dns": n.dns,
    })
}

/// `target <kind> <part> [...]`: where a named control is, in menu-local px:
/// `power <part>`, or `device <part> <fraction> <description...>`.
fn target(app: &mut App, args: &str) -> Value {
    let found = match args.split_once(' ') {
        Some(("power", part)) => app.popups().power.target(part.trim()),
        Some(("eq", profile)) => app.popups().eq.target(profile.trim()),
        Some(("traymenu", label)) => app.popups().tray.target(label.trim()),
        Some(("media", rest)) => rest.trim().split_once(' ').and_then(|(what, name)| {
            app.popups()
                .media
                .target((name.trim(), what), &app.renderer().media)
        }),
        Some(("trayicon", rest)) => tray_icon_target(app, rest),
        Some(("device", rest)) => {
            let mut parts = rest.splitn(3, ' ');
            match (
                parts.next(),
                parts.next().and_then(|f| f.parse::<f64>().ok()),
                parts.next(),
            ) {
                (Some(part), Some(fraction), Some(description)) => {
                    app.device_target((description, part, fraction))
                }
                _ => None,
            }
        }
        Some(("mixer", rest)) => {
            let mut parts = rest.splitn(3, ' ');
            match (
                parts.next(),
                parts.next().and_then(|f| f.parse::<f64>().ok()),
                parts.next(),
            ) {
                (Some(part), Some(fraction), Some(name)) => {
                    app.mixer_target((name, part, fraction))
                }
                _ => None,
            }
        }
        _ => None,
    };
    match found {
        Some((x, y)) => json!({ "ok": true, "x": x, "y": y }),
        None => error("target not found"),
    }
}

/// `target trayicon <output> <title...>`: the middle of the icon of the tray
/// item called `title` on that monitor's bar, bar-surface px.
fn tray_icon_target(app: &mut App, args: &str) -> Option<(f64, f64)> {
    let (name, title) = args.trim().split_once(' ')?;
    let targets: Vec<ClickTarget> = find_output(app, name)?.bar.as_ref()?.targets().to_vec();
    let tray = &app.renderer_mut().tray;
    targets.iter().find_map(|t| {
        let Action::TrayIcon(id) = t.action else {
            return None;
        };
        tray.item(id)
            .is_some_and(|i| i.title == title.trim())
            .then(|| (t.rect.x + t.rect.w / 2.0, t.rect.y + t.rect.h / 2.0))
    })
}

/// `menu-click <x> <y> [button]` / `menu-motion <x> <y>`, menu-local px.
fn menu_input(app: &mut App, command: &str, args: &str) -> Value {
    let Some(surface) = app.popups().latest_menu().map(|m| m.wl_surface().clone()) else {
        return error("no open menu");
    };
    let mut parts = args.split_whitespace();
    let x = parts.next().and_then(|v| v.parse::<f64>().ok());
    let y = parts.next().and_then(|v| v.parse::<f64>().ok());
    let (Some(x), Some(y)) = (x, y) else {
        return error("usage: menu-* <x> <y> [arg]");
    };
    match command {
        "menu-motion" => app.pointer_motion(&surface, x, y),
        // menu-scroll <x> <y> <+1|-1>: one wheel step, +1 = up.
        "menu-scroll" => {
            let up = parts
                .next()
                .and_then(|v| v.parse::<i32>().ok())
                .unwrap_or(0)
                > 0;
            app.pointer_scroll(&surface, x, y, up);
        }
        // menu-press: the button stays down (a drag can follow with
        // menu-motion, ended by menu-release); menu-click also releases.
        _ => {
            app.pointer_button(&surface, x, y, parse_button(parts.next().unwrap_or("left")));
            if command == "menu-click" {
                app.pointer_release();
            }
        }
    }
    json!({ "ok": true })
}

/// `catcher-click`: a left click on the first click-catcher.
fn catcher_click(app: &mut App) -> Value {
    let Some(surface) = app
        .popups()
        .catchers()
        .next()
        .map(|c| c.wl_surface().clone())
    else {
        return error("no catcher");
    };
    app.pointer_button(&surface, 5.0, 5.0, BTN_LEFT);
    json!({ "ok": true })
}

/// `cursor <output> <x> <y>`: which cursor the bar would show at a point on
/// that monitor's bar. Test bars never set a real cursor (they take no
/// pointer input), so the tests check the decision instead.
fn cursor(app: &App, args: &str) -> Value {
    let mut parts = args.split_whitespace();
    let name = parts.next().unwrap_or_default();
    let x = parts.next().and_then(|v| v.parse::<f64>().ok());
    let y = parts.next().and_then(|v| v.parse::<f64>().ok());
    let surface = find_output(app, name)
        .and_then(|o| o.bar.as_ref())
        .map(BarSurface::wl_surface);
    let (Some(surface), Some(x), Some(y)) = (surface, x, y) else {
        return error("usage: cursor <output> <x> <y>");
    };
    shape_reply(app.cursor_shape_at(surface, x, y))
}

/// `menu-cursor <x> <y>`: which cursor the bar would show over the open menu.
fn menu_cursor(app: &App, args: &str) -> Value {
    let mut parts = args.split_whitespace();
    let x = parts.next().and_then(|v| v.parse::<f64>().ok());
    let y = parts.next().and_then(|v| v.parse::<f64>().ok());
    let surface = app.popups().latest_menu().map(PopupSurface::wl_surface);
    let (Some(surface), Some(x), Some(y)) = (surface, x, y) else {
        return error("usage: menu-cursor <x> <y> (with a menu open)");
    };
    shape_reply(app.cursor_shape_at(surface, x, y))
}

/// The reply for a cursor shape: only the two shapes the bar uses have names.
fn shape_reply(shape: Shape) -> Value {
    let name = if shape == Shape::Pointer {
        "pointer"
    } else {
        "default"
    };
    json!({ "ok": true, "shape": name })
}

/// `left`/`right`/`middle` as a button code; anything else is left, as in C.
fn parse_button(name: &str) -> u32 {
    match name {
        "right" => BTN_RIGHT,
        "middle" => BTN_MIDDLE,
        _ => BTN_LEFT,
    }
}

/// `outputs`: every tracked monitor.
fn outputs(app: &App) -> Value {
    let list: Vec<Value> = app
        .outputs()
        .iter()
        .map(|output| {
            let (width, height) = app.output_size(output);
            json!({
                "name": output.name,
                "index": output.index,
                "width": width,
                "height": height,
                "has_bar": output.bar.is_some(),
            })
        })
        .collect();
    json!({ "ok": true, "outputs": list })
}

/// `rects <output>`: where each module's box was last drawn on that monitor's
/// bar, as `{"clock": [x, y, w, h], ...}`. Empty until the bar has drawn.
fn rects(app: &App, name: &str) -> Value {
    let Some(output) = find_output(app, name) else {
        return error("no such output");
    };
    let rects: serde_json::Map<String, Value> = output
        .bar
        .iter()
        .flat_map(BarSurface::boxes)
        .map(|b| {
            (
                b.name.to_owned(),
                json!([b.rect.x, b.rect.y, b.rect.w, b.rect.h]),
            )
        })
        .collect();
    json!({ "ok": true, "rects": rects })
}

/// A monitor by connector name (`DP-2`) or, if `name` starts with a digit, by index.
fn find_output<'a>(app: &'a App, name: &str) -> Option<&'a Output> {
    let index = name
        .starts_with(|c: char| c.is_ascii_digit())
        .then(|| name.parse::<usize>().ok());
    app.outputs()
        .iter()
        .find(|output| output.name == name || index.flatten() == Some(output.index))
}

/// The `{"ok":false,"error":...}` reply.
fn error(message: &str) -> Value {
    json!({ "ok": false, "error": message })
}

/// Writes one reply line to stdout and flushes it, so the test sees it at once.
/// (The `print_stdout` lint bans the print macros; writing to stdout is this
/// channel's whole job, so it goes through `io::Write` explicitly.)
fn send(reply: &Value) {
    let mut stdout = io::stdout().lock();
    if writeln!(stdout, "{reply}")
        .and_then(|()| stdout.flush())
        .is_err()
    {
        eprintln!("stickBar: test-control: couldn't write a reply to stdout");
    }
}
