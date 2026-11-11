//! Talking to Hyprland over its IPC sockets (the C bar's `hyprland/`; the
//! rules are in `docs/modules/hyprland.md`).
//!
//! Hyprland serves two Unix sockets from
//! `$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/`:
//!
//! - `.socket.sock`, the **request** socket (this file): one connection per
//!   command. Write the command, shut down the write side, read the reply to
//!   EOF. Hyprland only starts replying once it sees that EOF, so skipping the
//!   shutdown deadlocks. `j/<command>` replies in JSON.
//! - `.socket2.sock`, the **event** socket (`events.rs`): one persistent
//!   connection that Hyprland pushes `name>>payload` lines down.
//!
//! No Hyprland client library: these are two plain sockets.

pub(crate) mod dispatch;
pub(crate) mod events;

use std::{
    io::{Read, Write},
    net::Shutdown,
    os::unix::net::UnixStream,
    path::PathBuf,
    time::Duration,
};

/// How long a request may take before it's given up. Requests run on the main
/// loop (as in the C bar: they're quick and local), so a stuck Hyprland must
/// not be able to freeze the bar for longer than this.
const REQUEST_TIMEOUT: Duration = Duration::from_secs(1);

/// The path of Hyprland socket `name`, or `None` outside a Hyprland session.
pub(crate) fn socket_path(name: &str) -> Option<PathBuf> {
    let runtime_dir = std::env::var_os("XDG_RUNTIME_DIR")?;
    let instance = std::env::var_os("HYPRLAND_INSTANCE_SIGNATURE")?;
    Some(
        PathBuf::from(runtime_dir)
            .join("hypr")
            .join(instance)
            .join(name),
    )
}

/// Sends `command` on the request socket and returns the reply, or `None`
/// (with the reason logged) if Hyprland couldn't be reached.
pub(crate) fn request(command: &str) -> Option<String> {
    let Some(path) = socket_path(".socket.sock") else {
        eprintln!(
            "stickBar: cannot locate Hyprland's .socket.sock (XDG_RUNTIME_DIR or \
             HYPRLAND_INSTANCE_SIGNATURE unset -- not inside a Hyprland session?)"
        );
        return None;
    };
    let result = (|| {
        let mut stream = UnixStream::connect(&path)?;
        stream.set_read_timeout(Some(REQUEST_TIMEOUT))?;
        stream.set_write_timeout(Some(REQUEST_TIMEOUT))?;
        stream.write_all(command.as_bytes())?;
        // "That's the whole command": Hyprland replies only after this.
        stream.shutdown(Shutdown::Write)?;
        let mut reply = String::new();
        let _bytes: usize = stream.read_to_string(&mut reply)?;
        Ok::<String, std::io::Error>(reply)
    })();
    match result {
        Ok(reply) => Some(reply),
        Err(err) => {
            eprintln!("stickBar: Hyprland request '{command}' failed: {err}");
            None
        }
    }
}

/// Sends `j/<command>` and parses the JSON reply.
pub(crate) fn request_json(command: &str) -> Option<serde_json::Value> {
    let full = format!("j/{command}");
    let reply = request(&full)?;
    let json = serde_json::from_str(&reply).ok();
    if json.is_none() {
        eprintln!("stickBar: Hyprland request '{full}' returned invalid JSON");
    }
    json
}
