//! Running `blackpearl-eq.py profile NAME` (the apply half of the C bar's
//! `eq/eq_state.c`).
//!
//! A named thread starts the script and waits for it, which reaps it (C
//! double-forked for that), then reports the exit to the main loop. That
//! report is what lets "applying" end the moment a failed script exits,
//! instead of after C's 15 s guess.
//!
//! The script is started through `util::spawn` (empty signal mask, stdio on
//! `/dev/null`): a child of the bar must not inherit its blocked signals.

use std::{os::unix::ffi::OsStrExt, path::Path};

use nix::sys::wait::{WaitStatus, waitpid};

use crate::util::spawn::{Stdout, spawn};

use super::EqState;

/// The system interpreter: the script has no third-party dependencies, so no
/// `uv run` (which may not be on the PATH Hyprland starts the bar with). An
/// absolute path, since `posix_spawn` doesn't search the PATH.
const PYTHON_PATH: &str = "/usr/bin/python3";

impl EqState {
    /// Starts switching the DAC to profile `name` in the background and marks
    /// it pending. Does nothing while a switch is already under way: two
    /// scripts at once would steal each other's replies on the DAC's hidraw
    /// node (BUG.md's second EQ bug).
    pub(crate) fn apply(&mut self, name: &str) {
        if self.busy() {
            return;
        }
        let Some(exits) = self.exits.clone() else {
            eprintln!("stickBar: eq: profile switching is disabled for this run");
            return;
        };
        let (script, profile) = (self.script.clone(), name.to_owned());
        // Named, so it shows in `top -H` and the tests' thread checks. It
        // starts long after the signal setup in `main`, so it inherits the
        // signal block, like every other thread (the script itself doesn't).
        let started = std::thread::Builder::new()
            .name("eq-script".to_owned())
            .spawn(move || {
                run_script(&script, &profile);
                // The main loop handles this after `apply` has returned (both
                // run on the main thread), so `running` is set by then.
                if exits.send(()).is_err() {
                    eprintln!("stickBar: eq: the main loop stopped listening for the script");
                }
            });
        match started {
            // Detached: it ends by itself when the script does.
            Ok(_detached) => {
                self.running = true;
                self.pending = Some(name.to_owned());
            }
            Err(err) => eprintln!("stickBar: eq: couldn't start a thread for the script: {err}"),
        }
    }
}

/// Runs the script and waits for it (on the `eq-script` thread).
fn run_script(script: &Path, profile: &str) {
    let pid = match start(script, profile) {
        Ok(pid) => pid,
        Err(err) => {
            eprintln!("stickBar: eq: couldn't run {}: {err}", script.display());
            return;
        }
    };
    match waitpid(pid, None) {
        Ok(WaitStatus::Exited(_, 0)) => {}
        // A failure is reported on the script's own stderr, which goes
        // nowhere; the exit status at least says it happened.
        Ok(status) => eprintln!("stickBar: eq: 'profile {profile}' failed ({status:?})"),
        Err(err) => eprintln!("stickBar: eq: waiting for the script failed: {err}"),
    }
}

/// Starts `python3 SCRIPT profile NAME` (stdout thrown away: the script
/// reports through its state file).
fn start(script: &Path, profile: &str) -> Result<nix::unistd::Pid, String> {
    let args: [&[u8]; 4] = [
        PYTHON_PATH.as_bytes(),
        script.as_os_str().as_bytes(),
        b"profile",
        profile.as_bytes(),
    ];
    spawn(PYTHON_PATH, &args, Stdout::Null).map(|(pid, _no_pipe)| pid)
}
