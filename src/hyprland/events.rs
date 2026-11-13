//! Hyprland's event socket (the C bar's `hyprland/ipc_events.c`).
//!
//! One persistent non-blocking connection, watched by the event loop. Hyprland
//! writes `name>>payload\n` lines, but a line can arrive split across reads, so
//! bytes are buffered and only complete lines are handled. Only events that can
//! change which workspaces are occupied or focused matter; each read with any
//! of those triggers one re-query (`App::refresh_workspaces`), not one per line.
//!
//! When the connection drops (EOF or an error, e.g. Hyprland restarting), the
//! source removes itself and sets `App::hyprland_events_lost`. `main.rs` owns
//! the policy "reconnect exactly once, then give up on live updates", so it
//! lives in one place.

use std::{io::Read, os::unix::net::UnixStream};

use calloop::{Interest, LoopHandle, Mode, PostAction, generic::Generic};

use crate::wayland::App;

/// How much is read per wake-up.
const READ_CHUNK: usize = 8192;

/// Events after which the occupied/focused workspaces may differ.
const WORKSPACE_EVENTS: [&str; 12] = [
    "workspace",
    "workspacev2",
    "focusedmon",
    "focusedmonv2",
    "openwindow",
    "closewindow",
    "movewindow",
    "movewindowv2",
    "createworkspace",
    "createworkspacev2",
    "destroyworkspace",
    "destroyworkspacev2",
];

/// Connects to the event socket and adds it to the event loop.
pub(crate) fn start(handle: &LoopHandle<'static, App>) -> Result<(), String> {
    let path = super::socket_path(".socket2.sock").ok_or(
        "cannot locate Hyprland's .socket2.sock (XDG_RUNTIME_DIR or \
         HYPRLAND_INSTANCE_SIGNATURE unset -- not inside a Hyprland session?)",
    )?;
    let stream =
        UnixStream::connect(&path).map_err(|e| format!("connect(hyprland .socket2.sock): {e}"))?;
    stream.set_nonblocking(true).map_err(|e| e.to_string())?;
    let mut pending = Vec::new();
    let source = Generic::new(stream, Interest::READ, Mode::Level);
    handle
        .insert_source(source, move |_readiness, socket, app| {
            Ok(pump(socket, &mut pending, app))
        })
        .map(|_token| ())
        .map_err(|err| format!("couldn't watch Hyprland's event socket: {}", err.error))
}

/// Reads what's there and handles every complete line.
fn pump(stream: &UnixStream, pending: &mut Vec<u8>, app: &mut App) -> PostAction {
    let mut chunk = [0_u8; READ_CHUNK];
    // `Read` is implemented for `&UnixStream`, which is what the loop hands out.
    match (&*stream).read(&mut chunk) {
        Ok(0) => {
            // Hyprland closed the connection (e.g. it's restarting).
            app.hyprland_events_lost = true;
            return PostAction::Remove;
        }
        Ok(count) => pending.extend(chunk.iter().take(count)),
        Err(err) if err.kind() == std::io::ErrorKind::WouldBlock => return PostAction::Continue,
        Err(err) => {
            eprintln!("stickBar: read(hyprland events): {err}");
            app.hyprland_events_lost = true;
            return PostAction::Remove;
        }
    }
    let relevant = take_lines(pending)
        .iter()
        .any(|line| affects_workspaces(event_name(line)));
    if relevant {
        app.refresh_workspaces();
    }
    PostAction::Continue
}

/// Removes every complete line from the front of `buffer` and returns them; a
/// trailing partial line stays for the next read.
fn take_lines(buffer: &mut Vec<u8>) -> Vec<String> {
    let Some(last_newline) = buffer.iter().rposition(|&b| b == b'\n') else {
        return Vec::new();
    };
    let complete: Vec<u8> = buffer.drain(..=last_newline).collect();
    complete
        .split(|&b| b == b'\n')
        .filter(|line| !line.is_empty())
        .map(|line| String::from_utf8_lossy(line).into_owned())
        .collect()
}

/// The event name: everything before `>>` (the whole line if there's none).
fn event_name(line: &str) -> &str {
    line.split_once(">>").map_or(line, |(name, _payload)| name)
}

/// Whether event `name` can change the workspace pills.
fn affects_workspaces(name: &str) -> bool {
    WORKSPACE_EVENTS.contains(&name)
}

#[cfg(test)]
mod tests {
    use super::{affects_workspaces, event_name, take_lines};

    #[test]
    fn complete_lines_are_taken_partial_ones_kept() {
        let mut buffer = b"workspace>>3\nopenwindow>>a,3,kit".to_vec();
        assert_eq!(take_lines(&mut buffer), ["workspace>>3"]);
        assert_eq!(buffer, b"openwindow>>a,3,kit");
        buffer.extend(b"ty,kitty\nactivewindow>>x\n");
        assert_eq!(
            take_lines(&mut buffer),
            ["openwindow>>a,3,kitty,kitty", "activewindow>>x"]
        );
        assert_eq!(buffer, b"");
    }

    #[test]
    fn no_newline_means_no_lines() {
        let mut buffer = b"workspa".to_vec();
        assert_eq!(take_lines(&mut buffer), Vec::<String>::new());
        assert_eq!(buffer, b"workspa");
    }

    #[test]
    fn only_workspace_events_matter() {
        assert!(affects_workspaces(event_name("workspacev2>>3,3")));
        assert!(affects_workspaces(event_name("closewindow>>abc")));
        assert!(!affects_workspaces(event_name("activewindow>>kitty,title")));
        // A name that merely starts like a relevant one doesn't count.
        assert!(!affects_workspaces(event_name("workspacefoo>>1")));
    }
}
