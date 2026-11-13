//! Pending Arch updates (no C-bar or reference equivalent; added at the
//! user's request; `docs/modules/updates.md`). The widget is
//! `modules/updates/`.
//!
//! - **What's counted:** official-repo updates from `checkupdates` (pacman-contrib;
//!   it syncs a private copy of the package databases, so it never touches
//!   pacman's own lock) and AUR updates from `yay -Qua`. Both print one
//!   `name old -> new` line per package.
//! - **Where they run:** on a named `updates-check` thread, one check at a
//!   time, through `util::spawn` (empty signal mask, stdin on `/dev/null`).
//!   Both can take seconds (network), so never on the main thread. The result
//!   comes back on a calloop channel to `App::updates_result`.
//! - **When:** 60 s after start (the network is up by then, at login), then
//!   every 30 minutes, and 10 s after the local package database changes
//!   (inotify on `/var/lib/pacman/local`: a package installed, removed or
//!   upgraded), so the count drops right after you update. Never in test mode:
//!   test bars only check on the tests' `refresh updates`, with fake commands.
//! - **Failure** keeps the last good list and records the error, which the
//!   dropdown shows; one failing source doesn't throw away the other's list.

use std::{
    mem::MaybeUninit,
    os::fd::OwnedFd,
    path::Path,
    time::{Duration, Instant},
};

use calloop::{
    Interest, LoopHandle, Mode, PostAction, channel,
    generic::{Generic, NoIoDrop},
    timer::{TimeoutAction, Timer},
};
use rustix::{
    fs::inotify::{self, CreateFlags, WatchFlags},
    io::Errno,
};

use crate::{util::spawn::run_capture, wayland::App};

/// The official-repo check (pacman-contrib). Absolute: `posix_spawn`
/// doesn't search the PATH.
const CHECKUPDATES: &str = "/usr/bin/checkupdates";
/// The AUR helper; skipped if it isn't installed.
const YAY: &str = "/usr/bin/yay";
/// Overrides [`CHECKUPDATES`] (an absolute path), so tests run a fake.
const REPO_CMD_VAR: &str = "STICKBAR_TEST_UPDATES_REPO";
/// Overrides [`YAY`] (an absolute path; it's run with `-Qua`), so tests run a fake.
const AUR_CMD_VAR: &str = "STICKBAR_TEST_UPDATES_AUR";
/// The first check, after start: the network is up by then at login.
const FIRST_CHECK: Duration = Duration::from_secs(60);
/// Between regular checks.
const INTERVAL: Duration = Duration::from_mins(30);
/// After the package database changes, wait this long for the rest of the
/// transaction (an upgrade touches hundreds of entries) before re-checking.
const DB_SETTLE: Duration = Duration::from_secs(10);
/// pacman's installed-package database: one directory per package, replaced
/// on every install, removal and upgrade.
const PACMAN_LOCAL_DB: &str = "/var/lib/pacman/local";
/// `checkupdates`' exit code for "no updates" (not an error).
const CHECKUPDATES_NONE: i32 = 2;
/// Room for one read of inotify events.
const INOTIFY_BUF_BYTES: usize = 4096;

/// One pending update.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Pkg {
    /// The package name.
    pub(crate) name: String,
    /// The installed version.
    pub(crate) old: String,
    /// The available version.
    pub(crate) new: String,
}

/// What one check found: each source's list, or why it failed.
#[derive(Debug)]
pub(crate) struct CheckResult {
    /// `checkupdates`.
    repo: Result<Vec<Pkg>, String>,
    /// `yay -Qua`; `Ok(empty)` when yay isn't installed.
    aur: Result<Vec<Pkg>, String>,
}

/// The pending updates, as the last check found them.
#[derive(Debug, Default)]
pub(crate) struct Updates {
    /// Official-repo updates.
    pub(crate) repo: Vec<Pkg>,
    /// AUR updates.
    pub(crate) aur: Vec<Pkg>,
    /// A check is running now.
    pub(crate) checking: bool,
    /// When the last fully successful check finished.
    pub(crate) last_ok: Option<Instant>,
    /// Why the last check (partly) failed, if it did.
    pub(crate) last_error: Option<String>,
    /// A re-check after a package database change is already scheduled.
    settle_pending: bool,
    /// Where finished checks report (set by [`start`]).
    results: Option<channel::Sender<CheckResult>>,
}

impl Updates {
    /// How many updates are pending.
    pub(crate) fn total(&self) -> usize {
        self.repo.len() + self.aur.len()
    }

    /// Starts a check in the background, unless one is running. The result
    /// arrives at `App::updates_result`.
    pub(crate) fn check(&mut self) {
        if self.checking {
            return;
        }
        let Some(results) = self.results.clone() else {
            return; // `start` failed: checks are off for this run
        };
        let started = std::thread::Builder::new()
            .name("updates-check".to_owned())
            .spawn(move || {
                let result = CheckResult {
                    repo: check_repo(),
                    aur: check_aur(),
                };
                if results.send(result).is_err() {
                    eprintln!("stickBar: updates: the main loop stopped listening");
                }
            });
        match started {
            // Detached: it ends by itself when the commands do.
            Ok(_detached) => self.checking = true,
            Err(err) => eprintln!("stickBar: updates: couldn't start a check thread: {err}"),
        }
    }

    /// Takes in a finished check. A source that failed keeps its last list.
    pub(crate) fn finish(&mut self, result: CheckResult) {
        self.checking = false;
        let mut errors = Vec::new();
        match result.repo {
            Ok(list) => self.repo = list,
            Err(err) => errors.push(err),
        }
        match result.aur {
            Ok(list) => self.aur = list,
            Err(err) => errors.push(err),
        }
        if errors.is_empty() {
            self.last_ok = Some(Instant::now());
            self.last_error = None;
        } else {
            let error = errors.join("; ");
            eprintln!("stickBar: updates: {error}");
            self.last_error = Some(error);
        }
    }
}

/// Parses `name old -> new` lines (both commands' format). Anything else
/// (warnings, blank lines) is skipped.
pub(crate) fn parse(text: &str) -> Vec<Pkg> {
    text.lines()
        .filter_map(|line| {
            let mut words = line.split_whitespace();
            let (name, old, arrow, new) =
                (words.next()?, words.next()?, words.next()?, words.next()?);
            (arrow == "->").then(|| Pkg {
                name: name.to_owned(),
                old: old.to_owned(),
                new: new.to_owned(),
            })
        })
        .collect()
}

/// The command for `var`, or `default`.
fn command(var: &str, default: &str) -> String {
    std::env::var(var)
        .ok()
        .filter(|v| !v.is_empty())
        .unwrap_or_else(|| default.to_owned())
}

/// Runs `checkupdates`: exit 0 lists updates, 2 means none, anything else failed.
fn check_repo() -> Result<Vec<Pkg>, String> {
    let path = command(REPO_CMD_VAR, CHECKUPDATES);
    let (code, out) = run_capture(&path, &[path.as_bytes()])?;
    match code {
        0 => Ok(parse(&out)),
        CHECKUPDATES_NONE => Ok(Vec::new()),
        _ => Err(format!("checkupdates failed (exit {code})")),
    }
}

/// Runs `yay -Qua`. Like `pacman -Qu`, it exits 1 when there's nothing to
/// update, so 1 with no output is "none", not a failure.
fn check_aur() -> Result<Vec<Pkg>, String> {
    let path = command(AUR_CMD_VAR, YAY);
    if !Path::new(&path).exists() {
        return Ok(Vec::new()); // no AUR helper: nothing to count
    }
    let (code, out) = run_capture(&path, &[path.as_bytes(), b"-Qua"])?;
    let list = parse(&out);
    match code {
        0 => Ok(list),
        1 if list.is_empty() => Ok(Vec::new()),
        _ => Err(format!("yay -Qua failed (exit {code})")),
    }
}

/// Joins the updates check to the main loop: the channel results come back
/// on and, outside test mode, the timers and the package database watch.
pub(crate) fn start(handle: &LoopHandle<'static, App>, updates: &mut Updates, test_mode: bool) {
    let (results, source) = channel::channel::<CheckResult>();
    let inserted = handle.insert_source(source, |event, (), app| {
        if let channel::Event::Msg(result) = event {
            app.updates_result(result);
        }
    });
    if let Err(err) = inserted {
        eprintln!(
            "stickBar: updates: couldn't listen for results ({}) -- update checks disabled",
            err.error
        );
        return;
    }
    updates.results = Some(results);
    if test_mode {
        return; // only the tests' `refresh updates` checks
    }

    let timer = handle.insert_source(Timer::from_duration(FIRST_CHECK), |_deadline, (), app| {
        app.check_updates();
        TimeoutAction::ToDuration(INTERVAL)
    });
    if let Err(err) = timer {
        eprintln!("stickBar: updates: couldn't start the timer: {}", err.error);
    }
    if let Err(err) = watch_db(handle) {
        eprintln!("stickBar: updates: {err} -- no re-check after updating");
    }
}

/// Watches pacman's local database: any package directory created or
/// removed (an install, removal or upgrade) schedules one re-check
/// [`DB_SETTLE`] later; more changes meanwhile don't schedule another.
fn watch_db(handle: &LoopHandle<'static, App>) -> Result<(), String> {
    let fd = inotify::init(CreateFlags::NONBLOCK | CreateFlags::CLOEXEC)
        .map_err(|err| format!("inotify_init1: {err}"))?;
    let _watch = inotify::add_watch(
        &fd,
        PACMAN_LOCAL_DB,
        WatchFlags::CREATE | WatchFlags::DELETE | WatchFlags::MOVED_TO,
    )
    .map_err(|err| format!("inotify_add_watch {PACMAN_LOCAL_DB}: {err}"))?;
    let timers = handle.clone();
    let source = Generic::new(fd, Interest::READ, Mode::Level);
    handle
        .insert_source(source, move |_readiness, inotify_fd, app| {
            drain(inotify_fd);
            if app.updates_settle_start() {
                let settle =
                    timers.insert_source(Timer::from_duration(DB_SETTLE), |_, (), settled_app| {
                        settled_app.updates_settle_done();
                        TimeoutAction::Drop
                    });
                if let Err(err) = settle {
                    eprintln!(
                        "stickBar: updates: couldn't schedule a re-check: {}",
                        err.error
                    );
                }
            }
            Ok(PostAction::Continue)
        })
        .map(|_token| ())
        .map_err(|err| format!("couldn't watch the inotify fd: {}", err.error))
}

impl Updates {
    /// The package database changed: returns true if a re-check should be
    /// scheduled now (none is pending yet).
    pub(crate) const fn settle_start(&mut self) -> bool {
        let start = !self.settle_pending;
        self.settle_pending = true;
        start
    }

    /// The settle delay is over: check now.
    pub(crate) fn settle_done(&mut self) {
        self.settle_pending = false;
        self.check();
    }
}

/// Reads every waiting inotify event (only that something changed matters).
fn drain(fd: &NoIoDrop<OwnedFd>) {
    let mut buf = [const { MaybeUninit::<u8>::uninit() }; INOTIFY_BUF_BYTES];
    let mut reader = inotify::Reader::new(&**fd, &mut buf);
    loop {
        match reader.next() {
            Ok(_) | Err(Errno::INTR) => {}
            // AGAIN: drained (the fd is non-blocking).
            Err(Errno::AGAIN) => return,
            Err(err) => {
                eprintln!("stickBar: updates: reading inotify events failed: {err}");
                return;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{CheckResult, Pkg, Updates, parse};

    fn pkg(name: &str) -> Pkg {
        Pkg {
            name: name.to_owned(),
            old: "1".to_owned(),
            new: "2".to_owned(),
        }
    }

    #[test]
    fn parses_update_lines_and_skips_the_rest() {
        let text = "linux 6.17.1-1 -> 6.17.2-1\n\
                    :: warning: something\n\
                    \n\
                    mesa 1:25.2.4-1 -> 1:25.2.5-1\n\
                    broken line\n";
        let list = parse(text);
        assert_eq!(list.len(), 2);
        assert_eq!(list.first().map(|p| p.name.as_str()), Some("linux"));
        assert_eq!(list.get(1).map(|p| p.new.as_str()), Some("1:25.2.5-1"));
    }

    #[test]
    fn a_failed_source_keeps_its_last_list() {
        let mut updates = Updates::default();
        updates.finish(CheckResult {
            repo: Ok(vec![pkg("a"), pkg("b")]),
            aur: Ok(vec![pkg("c")]),
        });
        assert_eq!(updates.total(), 3);
        assert!(updates.last_error.is_none() && updates.last_ok.is_some());
        updates.finish(CheckResult {
            repo: Err("offline".to_owned()),
            aur: Ok(Vec::new()),
        });
        assert_eq!((updates.repo.len(), updates.aur.len()), (2, 0));
        assert_eq!(updates.last_error.as_deref(), Some("offline"));
    }
}
