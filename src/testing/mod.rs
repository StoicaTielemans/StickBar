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
//! anything else          -> {"ok":false,"error":"unknown command"}
//! ```

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

use crate::wayland::{App, Output};

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
fn dispatch(app: &App, line: &str) -> Value {
    let (command, rest) = line.split_once(' ').unwrap_or((line, ""));
    let rest = rest.trim();
    match command {
        "ping" => json!({ "ok": true }),
        "outputs" => outputs(app),
        "rects" if !rest.is_empty() => rects(app, rest),
        _ => error("unknown command"),
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
/// bar. No module is ported yet, so the map is empty, which is also what the C
/// bar replies for a monitor with nothing drawn on it.
fn rects(app: &App, name: &str) -> Value {
    if find_output(app, name).is_none() {
        return error("no such output");
    }
    json!({ "ok": true, "rects": {} })
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
