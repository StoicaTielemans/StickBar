//! Starting a child process the safe way (shared by the EQ's profile script
//! and the updates check).
//!
//! **Why `posix_spawn` and not `std::process::Command`:** the bar blocks
//! SIGINT, SIGTERM and SIGUSR1 in every thread for its signal source, and a
//! blocked mask survives exec. `Command` doesn't reset it (only SIGPIPE's
//! handler), so a child would start unable to be stopped by SIGTERM or
//! Ctrl-C (`docs/modules/eq.md`, the rule a child of the bar must follow).
//! Resetting it with `Command::pre_exec` needs `unsafe`; nix's safe
//! `posix_spawn` wrapper takes the mask as an attribute instead.
//!
//! Every child gets: an empty signal mask, SIGPIPE back to its default action
//! (Rust's runtime ignores it in the bar), the bar's environment, and stdin
//! and stderr on `/dev/null`. **stdin too**: in test mode the bar's stdin is
//! the test-control channel, which a child must never read from. stdout is
//! either `/dev/null` or a pipe the caller reads ([`Stdout`]).

use std::{
    ffi::CString,
    fs::File,
    io::Read,
    os::{
        fd::{AsRawFd, OwnedFd},
        unix::ffi::{OsStrExt, OsStringExt},
    },
};

use nix::{
    spawn::{PosixSpawnAttr, PosixSpawnFileActions, PosixSpawnFlags, posix_spawn},
    sys::{
        signal::{SigSet, Signal},
        wait::{WaitStatus, waitpid},
    },
    unistd::Pid,
};
use rustix::pipe::{PipeFlags, pipe_with};

/// Where a child's stdout goes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Stdout {
    /// Thrown away.
    Null,
    /// Into a pipe, returned to the caller.
    Pipe,
}

/// Starts `path` (absolute: `posix_spawn` doesn't search the PATH) with
/// `args` (`args[0]` is the program's own name, as with `execv`). Returns its
/// pid and, for [`Stdout::Pipe`], the read end of its stdout.
pub(crate) fn spawn(
    path: &str,
    args: &[&[u8]],
    stdout: Stdout,
) -> Result<(Pid, Option<OwnedFd>), String> {
    let args = args
        .iter()
        .map(|a| CString::new(a.to_vec()).map_err(|_nul| "a NUL byte in an argument".to_owned()))
        .collect::<Result<Vec<_>, _>>()?;
    // posix_spawn takes the environment explicitly: pass the bar's own on
    // (HOME, XDG_CACHE_HOME, PATH... the child may rely on any of them).
    let env: Vec<CString> = std::env::vars_os()
        .filter_map(|(key, value)| {
            let mut entry = key.into_vec();
            entry.push(b'=');
            entry.extend_from_slice(value.as_bytes());
            CString::new(entry).ok()
        })
        .collect();

    // Opened close-on-exec (std's default): only the dup2'd copies reach the
    // child. Everything below must stay open until posix_spawn returns.
    let devnull = File::options()
        .read(true)
        .write(true)
        .open("/dev/null")
        .map_err(|err| format!("/dev/null: {err}"))?;
    // Close-on-exec on both ends too, so the child only keeps the write end
    // as its fd 1 (a stray inherited copy would delay our EOF until every
    // grandchild that inherited it exits).
    let pipe_ends = match stdout {
        Stdout::Null => None,
        Stdout::Pipe => Some(pipe_with(PipeFlags::CLOEXEC).map_err(|e| format!("pipe: {e}"))?),
    };
    let mut actions = PosixSpawnFileActions::init().map_err(|e| e.to_string())?;
    let out_fd = pipe_ends
        .as_ref()
        .map_or_else(|| devnull.as_raw_fd(), |(_read, write)| write.as_raw_fd());
    for (fd, from) in [
        (0, devnull.as_raw_fd()),
        (1, out_fd),
        (2, devnull.as_raw_fd()),
    ] {
        actions.add_dup2(from, fd).map_err(|e| e.to_string())?;
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

    let pid = posix_spawn(path, &actions, &attr, &args, &env).map_err(|e| e.to_string())?;
    // Our copy of the write end closes here (dropped with the tuple), so the
    // reader sees EOF when the child exits.
    Ok((pid, pipe_ends.map(|(read, _write)| read)))
}

/// Runs `path` with `args`, reads all of its stdout, waits for it, and
/// returns (exit code, stdout). Blocks: call it from a worker thread. A child
/// killed by a signal reports exit code -1.
pub(crate) fn run_capture(path: &str, args: &[&[u8]]) -> Result<(i32, String), String> {
    let (pid, read_end) = spawn(path, args, Stdout::Pipe)?;
    let mut out = Vec::new();
    if let Some(read_end) = read_end {
        // Read before waiting: a child writing more than the pipe holds
        // (64 KiB) would otherwise block forever on a full pipe.
        let _bytes = File::from(read_end)
            .read_to_end(&mut out)
            .map_err(|e| format!("reading {path}'s output: {e}"))?;
    }
    let code = match waitpid(pid, None).map_err(|e| format!("waiting for {path}: {e}"))? {
        WaitStatus::Exited(_, code) => code,
        // Killed by a signal; the rest can't come from a plain waitpid on an
        // exited child, but count as "no exit code" too.
        WaitStatus::Signaled(..)
        | WaitStatus::Stopped(..)
        | WaitStatus::PtraceEvent(..)
        | WaitStatus::PtraceSyscall(_)
        | WaitStatus::Continued(_)
        | WaitStatus::StillAlive => -1,
    };
    Ok((code, String::from_utf8_lossy(&out).into_owned()))
}
