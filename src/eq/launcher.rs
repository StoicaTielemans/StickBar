//! Running `blackpearl-eq.py profile NAME` (the apply half of the C bar's
//! `eq/eq_state.c`).
//!
//! A named thread starts the script and waits for it, which reaps it (C
//! double-forked for that), then reports the exit to the main loop. That
//! report is what lets "applying" end the moment a failed script exits,
//! instead of after C's 15 s guess.
//!
//! **Why `posix_spawn` and not `std::process::Command`:** the bar blocks
//! SIGINT, SIGTERM and SIGUSR1 in every thread for its signal source, and a
//! blocked mask survives exec. `Command` doesn't reset it (only SIGPIPE's
//! handler), so the script would start unable to be stopped by SIGTERM or
//! Ctrl-C. Resetting it with `Command::pre_exec` needs `unsafe`; nix's safe
//! `posix_spawn` wrapper takes the mask as an attribute instead. The C bar
//! reset it by hand in the child.

use std::{
    ffi::CString,
    fs::File,
    os::{
        fd::AsRawFd,
        unix::ffi::{OsStrExt, OsStringExt},
    },
    path::Path,
};

use nix::{
    spawn::{PosixSpawnAttr, PosixSpawnFileActions, PosixSpawnFlags, posix_spawn},
    sys::{
        signal::{SigSet, Signal},
        wait::{WaitStatus, waitpid},
    },
    unistd::Pid,
};

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
    let pid = match spawn(script, profile) {
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

/// Starts `python3 SCRIPT profile NAME` with an empty signal mask, SIGPIPE
/// back to its default action (Rust's runtime ignores it in the bar), the
/// bar's environment, and stdin/stdout/stderr on `/dev/null`. **stdin too**:
/// in test mode the bar's stdin is the test-control channel.
fn spawn(script: &Path, profile: &str) -> Result<Pid, String> {
    let c_string =
        |bytes: Vec<u8>| CString::new(bytes).map_err(|_nul| "a NUL byte in an argument".to_owned());
    let args = [
        PYTHON_PATH.as_bytes().to_vec(),
        script.as_os_str().as_bytes().to_vec(),
        b"profile".to_vec(),
        profile.as_bytes().to_vec(),
    ]
    .into_iter()
    .map(c_string)
    .collect::<Result<Vec<_>, _>>()?;
    // posix_spawn takes the environment explicitly: pass the bar's own on
    // (HOME and XDG_CACHE_HOME decide where the script writes its state).
    let env: Vec<CString> = std::env::vars_os()
        .filter_map(|(key, value)| {
            let mut entry = key.into_vec();
            entry.push(b'=');
            entry.extend_from_slice(value.as_bytes());
            CString::new(entry).ok()
        })
        .collect();

    // Opened close-on-exec (std's default): only the dup2'd copies reach the
    // script. It must stay open until posix_spawn returns.
    let devnull = File::options()
        .read(true)
        .write(true)
        .open("/dev/null")
        .map_err(|err| format!("/dev/null: {err}"))?;
    let mut actions = PosixSpawnFileActions::init().map_err(|e| e.to_string())?;
    for fd in 0..=2 {
        actions
            .add_dup2(devnull.as_raw_fd(), fd)
            .map_err(|e| e.to_string())?;
    }

    let mut attr = PosixSpawnAttr::init().map_err(|e| e.to_string())?;
    attr.set_sigmask(&SigSet::empty())
        .map_err(|e| e.to_string())?;
    let mut sigpipe = SigSet::empty();
    sigpipe.add(Signal::SIGPIPE);
    attr.set_sigdefault(&sigpipe).map_err(|e| e.to_string())?;
    // The two attributes above only apply with their flags set.
    attr.set_flags(
        PosixSpawnFlags::POSIX_SPAWN_SETSIGMASK | PosixSpawnFlags::POSIX_SPAWN_SETSIGDEF,
    )
    .map_err(|e| e.to_string())?;

    posix_spawn(PYTHON_PATH, &actions, &attr, &args, &env).map_err(|e| e.to_string())
}
