//! Sending Hyprland dispatchers (the C bar's `hyprland/dispatch.c`).
//!
//! The dispatch syntax depends on the user's Hyprland config. A Lua-configured
//! Hyprland (this machine's) wants `dispatch hl.dsp.focus({ workspace = 3 })`,
//! a classic one `dispatch workspace 3`. Hard-won details (BUG.md, "Hyprland IPC"):
//!
//! - **No quotes around the Lua form.** `hyprctl dispatch '...'` works on the
//!   command line only because the shell strips the quotes; on the raw socket
//!   they're literal and Hyprland rejects them.
//! - The Lua form is tried first. If its reply starts with `error`, the classic
//!   form is sent, and the rejection is logged once.
//! - Which form worked is **cached** for the rest of the run (a Hyprland
//!   instance doesn't change syntax), so a click never costs two requests
//!   after the first.
//! - **A missing reply caches nothing.** Once, a failed request (no reply at
//!   all) was treated as "the Lua form works", which stuck a classic-syntax
//!   Hyprland on the wrong form for the whole run.
//! - `hl.dsp.exec_raw(...)` looks like the config-agnostic escape hatch, but it
//!   replied `ok` without switching on this machine's build. Don't use it
//!   without re-testing it against the socket.

/// Which dispatch syntax this Hyprland accepts, once known.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Style {
    /// Not found out yet: try Lua first.
    Unknown,
    /// The Lua form works.
    Lua,
    /// Only the classic form works.
    Classic,
}

/// Sends dispatchers, remembering which syntax works.
#[derive(Debug)]
pub(crate) struct Dispatcher {
    /// What's known about the syntax so far.
    style: Style,
}

impl Dispatcher {
    /// A dispatcher that hasn't sent anything yet.
    pub(crate) const fn new() -> Self {
        Self {
            style: Style::Unknown,
        }
    }

    /// Focuses workspace `id`.
    pub(crate) fn focus_workspace(&mut self, id: i32) {
        let lua = format!("dispatch hl.dsp.focus({{ workspace = {id} }})");
        let classic = format!("dispatch workspace {id}");
        self.send(
            &lua,
            &classic,
            &format!("focusing workspace {id}"),
            super::request,
        );
    }

    /// Sends `lua`, or `classic` if that's rejected (or known to be needed).
    /// `request` sends one command and returns the reply (`None` = no reply);
    /// it's a parameter so the caching rules can be unit tested.
    fn send(
        &mut self,
        lua: &str,
        classic: &str,
        what: &str,
        request: impl Fn(&str) -> Option<String>,
    ) {
        if self.style != Style::Classic {
            let Some(reply) = request(lua) else {
                return; // no reply: the request already logged why; learn nothing
            };
            if !is_error(&reply) {
                self.style = Style::Lua;
                return;
            }
            if self.style == Style::Unknown {
                eprintln!(
                    "stickBar: Hyprland rejected the Lua dispatch syntax ({}), falling back \
                     to the classic form for the rest of this run",
                    reply.trim()
                );
            }
            self.style = Style::Classic;
        }
        if let Some(reply) = request(classic)
            && is_error(&reply)
        {
            eprintln!(
                "stickBar: Hyprland rejected {what} in both syntaxes ({})",
                reply.trim()
            );
        }
    }
}

/// Whether a dispatch reply is Hyprland's rejection.
fn is_error(reply: &str) -> bool {
    reply.starts_with("error")
}

#[cfg(test)]
mod tests {
    use std::cell::RefCell;

    use super::{Dispatcher, Style};

    /// Runs one `send` against a fake Hyprland that answers `reply(command)`,
    /// returning the commands it received.
    fn run(dispatcher: &mut Dispatcher, reply: impl Fn(&str) -> Option<String>) -> Vec<String> {
        let sent = RefCell::new(Vec::new());
        dispatcher.send("LUA", "CLASSIC", "test", |cmd| {
            sent.borrow_mut().push(cmd.to_owned());
            reply(cmd)
        });
        sent.into_inner()
    }

    #[test]
    fn lua_accepted_is_cached() {
        let mut d = Dispatcher::new();
        assert_eq!(run(&mut d, |_| Some("ok".into())), ["LUA"]);
        assert_eq!(d.style, Style::Lua);
    }

    #[test]
    fn lua_rejected_falls_back_and_sticks_to_classic() {
        let mut d = Dispatcher::new();
        let reply = |cmd: &str| Some(if cmd == "LUA" { "error: nope" } else { "ok" }.to_owned());
        assert_eq!(run(&mut d, reply), ["LUA", "CLASSIC"]);
        assert_eq!(run(&mut d, reply), ["CLASSIC"]); // cached: no second probe
    }

    #[test]
    fn no_reply_caches_nothing() {
        let mut d = Dispatcher::new();
        assert_eq!(run(&mut d, |_| None), ["LUA"]);
        assert_eq!(d.style, Style::Unknown); // the BUG.md regression
    }
}
