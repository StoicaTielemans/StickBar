//! A read-only mirror of the TRN Black Pearl DAC's EQ (the C bar's
//! `eq/eq_state.c`), as last written by `DacController`'s `blackpearl-eq.py`.
//! The whole story is in `docs/modules/eq.md`; the short version:
//!
//! - **The bar never talks to the DAC.** The script owns its HID protocol, and a
//!   second process reading the DAC while the script writes it steals replies on
//!   the shared hidraw node. So the script writes a JSON snapshot (`state.json`)
//!   after every command, and the bar only ever reads that file.
//! - **Live updates through inotify, not polling.** The watch is on the
//!   *directory*, not the file: the script replaces `state.json` by writing a
//!   temp file and renaming it over, which would orphan a watch on the old
//!   file's inode after the first update. Idle cost: nothing at all.
//! - **Applying a profile** runs the script (`launcher.rs`). "applying" is shown
//!   from the click until the state write arrives or the script exits, whichever
//!   comes first, and no second script starts while one runs (two scripts at
//!   once would steal each other's replies). Both were open bugs in the C bar,
//!   which guessed with a 15 s timeout instead (BUG.md, "EQ").
//!
//! Drawing the curve is in `curve.rs`, kept apart from the plumbing as in C.

pub(crate) mod curve;
mod launcher;

use std::{
    fs::{self, File},
    io::Read,
    mem::MaybeUninit,
    os::fd::OwnedFd,
    path::{Path, PathBuf},
};

use calloop::{
    Interest, LoopHandle, Mode, PostAction, channel,
    generic::{Generic, NoIoDrop},
};
use rustix::{
    fs::inotify::{self, CreateFlags, WatchFlags},
    io::Errno,
};
use serde_json::Value;

use crate::wayland::App;

/// The state file's name inside the state directory. The script's own
/// `state.json.tmp` shows up in the same directory and is ignored.
const STATE_FILE_NAME: &str = "state.json";
/// The real file is about 3 KB; anything bigger than this is garbage.
const STATE_FILE_MAX_BYTES: u64 = 256 * 1024;
/// The most curve points kept (the script writes 64); extra ones are ignored.
const MAX_CURVE_POINTS: usize = 128;
/// Where the `DacController` checkout lives, relative to `$HOME`. Overridable
/// with `BLACKPEARL_EQ_SCRIPT` (an absolute path), which the tests use to run
/// a fake instead.
const DEFAULT_SCRIPT_REL: &str = "Code/github.com/StoicaTielemans/DacController/blackpearl-eq.py";
/// Room for one read of inotify events (each is 16 bytes plus the name).
const INOTIFY_BUF_BYTES: usize = 4096;

/// What `state.json` said, as last read. Zeroed (`available: false`, EQ
/// "off") until a valid file has been read, like the C bar's static snapshot.
#[derive(Clone, Debug, Default)]
pub(crate) struct Snapshot {
    /// A valid state file has been read.
    pub(crate) available: bool,
    /// The preset name, or "" when the EQ matches no preset.
    pub(crate) profile: String,
    /// `low` / `high`, or "" when unknown.
    pub(crate) gain: String,
    /// The preamp in dB.
    pub(crate) preamp: f64,
    /// Whether the EQ is on (`blackpearl-eq.py off` turns it off).
    pub(crate) enabled: bool,
    /// The response as (Hz, dB) points, log-spaced over 20 Hz..20 kHz,
    /// preamp not included.
    pub(crate) curve: Vec<(f64, f64)>,
    /// The curve's highest point, dB.
    pub(crate) peak_db: f64,
    /// The curve's lowest point, dB.
    pub(crate) min_db: f64,
}

/// The EQ module's data: the snapshot, plus the profile switch in progress.
#[derive(Debug, Default)]
pub(crate) struct EqState {
    /// What the state file last said.
    pub(crate) snap: Snapshot,
    /// The profile being applied, shown as the yellow chip / "applying" pill, until
    /// the state write arrives or the script exits.
    pub(crate) pending: Option<String>,
    /// A script is running. Clicks on profiles are ignored meanwhile.
    running: bool,
    /// `$XDG_CACHE_HOME/blackpearl-eq/state.json`.
    state_file: PathBuf,
    /// The script to run (`BLACKPEARL_EQ_SCRIPT` or the default checkout).
    script: PathBuf,
    /// Where a finished script's thread reports back (set by [`start`]).
    exits: Option<channel::Sender<()>>,
}

impl EqState {
    /// Works out the paths (the script's own rule for where the state goes),
    /// creates the state directory so the watch can be armed before the script
    /// has ever run, and reads the current state, if any.
    pub(crate) fn new() -> Self {
        let home = std::env::var_os("HOME")
            .map(PathBuf::from)
            .unwrap_or_default();
        let cache = std::env::var_os("XDG_CACHE_HOME")
            .filter(|v| !v.is_empty())
            .map_or_else(|| home.join(".cache"), PathBuf::from);
        let dir = cache.join("blackpearl-eq");
        if let Err(err) = fs::create_dir_all(&dir) {
            eprintln!("stickBar: eq: mkdir {}: {err}", dir.display());
        }
        let script = std::env::var_os("BLACKPEARL_EQ_SCRIPT")
            .filter(|v| !v.is_empty())
            .map_or_else(|| home.join(DEFAULT_SCRIPT_REL), PathBuf::from);
        let mut eq = Self {
            state_file: dir.join(STATE_FILE_NAME),
            script,
            ..Self::default()
        };
        // Fine if this fails: the script may simply never have run yet.
        let _loaded = eq.load();
        eq
    }

    /// Re-reads the state file. The snapshot is swapped in only if it parsed,
    /// so a broken file leaves the last good state on screen. Any successful
    /// read ends "applying": the script finished, or something else changed
    /// the EQ, and either way nothing is pending from the bar's point of view.
    /// Returns whether a new snapshot was read.
    pub(crate) fn load(&mut self) -> bool {
        let Some(snap) = read_state(&self.state_file) else {
            return false;
        };
        self.snap = snap;
        self.pending = None;
        true
    }

    /// A profile switch is under way: profile clicks do nothing now.
    pub(crate) const fn busy(&self) -> bool {
        self.running || self.pending.is_some()
    }

    /// The script that ran `apply` has exited: "applying" ends (at once, if it
    /// failed without writing state: BUG.md's first EQ bug), and the state is
    /// read again in case the exit came through before the inotify event.
    pub(crate) fn script_exited(&mut self) {
        self.running = false;
        self.pending = None;
        let _loaded = self.load();
    }

    /// The state directory, for the inotify watch.
    fn state_dir(&self) -> &Path {
        self.state_file.parent().unwrap_or_else(|| Path::new("."))
    }
}

/// Reads and parses the state file; `None` if it's missing or not JSON.
fn read_state(path: &Path) -> Option<Snapshot> {
    let mut text = String::new();
    let _bytes = File::open(path)
        .ok()?
        .take(STATE_FILE_MAX_BYTES)
        .read_to_string(&mut text)
        .ok()?;
    parse(&text)
}

/// Parses `state.json`'s text. Missing fields take the C bar's defaults (a
/// null profile, e.g. an EQ that matches no preset, is ""; `enabled` is true).
fn parse(text: &str) -> Option<Snapshot> {
    let root: Value = serde_json::from_str(text).ok()?;
    let string = |key: &str| {
        root.get(key)
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_owned()
    };
    let number = |key: &str| root.get(key).and_then(Value::as_f64).unwrap_or(0.0);
    let curve_obj = root.get("curve");
    let points = |key: &str| {
        curve_obj
            .and_then(|c| c.get(key))
            .and_then(Value::as_array)
            .map(|a| {
                a.iter()
                    .map(|v| v.as_f64().unwrap_or(0.0))
                    .collect::<Vec<_>>()
            })
            .unwrap_or_default()
    };
    // Only both arrays together make a curve (the shorter one's length).
    let curve = points("freqs")
        .into_iter()
        .zip(points("db"))
        .take(MAX_CURVE_POINTS)
        .collect();
    Some(Snapshot {
        available: true,
        profile: string("profile"),
        gain: string("gain"),
        preamp: number("preamp"),
        enabled: root.get("enabled").and_then(Value::as_bool).unwrap_or(true),
        curve,
        peak_db: number("peak_db"),
        min_db: number("min_db"),
    })
}

/// Joins the EQ to the main loop: the inotify watch on the state directory,
/// and the channel finished scripts report on. Without inotify the module
/// still shows what was read at startup, it just won't follow changes for
/// this run (the C bar's "degrade gracefully").
pub(crate) fn start(handle: &LoopHandle<'static, App>, eq: &mut EqState) {
    let (exits, exit_source) = channel::channel::<()>();
    let inserted = handle.insert_source(exit_source, |event, (), app| {
        if let channel::Event::Msg(()) = event {
            app.eq_script_exited();
        }
    });
    match inserted {
        Ok(_token) => eq.exits = Some(exits),
        Err(err) => eprintln!(
            "stickBar: eq: couldn't watch for finished scripts ({}) -- profile switching disabled",
            err.error
        ),
    }

    if let Err(err) = watch(handle, eq.state_dir()) {
        eprintln!("stickBar: eq: {err} -- EQ live updates disabled for this run");
    }
}

/// Adds the inotify watch on `dir` to the loop.
fn watch(handle: &LoopHandle<'static, App>, dir: &Path) -> Result<(), String> {
    let fd = inotify::init(CreateFlags::NONBLOCK | CreateFlags::CLOEXEC)
        .map_err(|err| format!("inotify_init1: {err}"))?;
    // MOVED_TO is what the script's rename-based writes fire; CLOSE_WRITE also
    // covers rewriting the file in place (e.g. editing it by hand to test).
    let _watch = inotify::add_watch(&fd, dir, WatchFlags::CLOSE_WRITE | WatchFlags::MOVED_TO)
        .map_err(|err| format!("inotify_add_watch {}: {err}", dir.display()))?;
    // Level-triggered: `drain` reads until the fd is empty each time anyway.
    let source = Generic::new(fd, Interest::READ, Mode::Level);
    handle
        .insert_source(source, |_readiness, inotify_fd, app| {
            if drain(inotify_fd) {
                app.eq_file_changed();
            }
            Ok(PostAction::Continue)
        })
        .map(|_token| ())
        .map_err(|err| format!("couldn't watch the inotify fd: {}", err.error))
}

/// Reads every waiting inotify event. Returns whether one was about
/// `state.json` itself.
fn drain(fd: &NoIoDrop<OwnedFd>) -> bool {
    let mut buf = [const { MaybeUninit::<u8>::uninit() }; INOTIFY_BUF_BYTES];
    let mut reader = inotify::Reader::new(&**fd, &mut buf);
    let mut touched = false;
    loop {
        match reader.next() {
            Ok(event) => {
                if event
                    .file_name()
                    .is_some_and(|name| name.to_bytes() == STATE_FILE_NAME.as_bytes())
                {
                    touched = true;
                }
            }
            Err(Errno::INTR) => {}
            // AGAIN: drained (the fd is non-blocking).
            Err(Errno::AGAIN) => return touched,
            Err(err) => {
                eprintln!("stickBar: eq: reading inotify events failed: {err}");
                return touched;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::parse;

    #[test]
    fn parses_the_scripts_state() {
        let snap = parse(
            r#"{"profile":"zero2","gain":"low","preamp":-3.0,"enabled":true,
                "curve":{"freqs":[20,200,20000],"db":[4,2]},"peak_db":4,"min_db":0}"#,
        )
        .unwrap();
        assert!(snap.available && snap.enabled);
        assert_eq!(
            (snap.profile.as_str(), snap.gain.as_str()),
            ("zero2", "low")
        );
        assert_eq!(snap.curve, vec![(20.0, 4.0), (200.0, 2.0)]);
        assert!((snap.preamp + 3.0).abs() < 1e-9 && (snap.peak_db - 4.0).abs() < 1e-9);
    }

    #[test]
    fn defaults_and_garbage() {
        let snap = parse(r#"{"profile":null}"#).unwrap();
        assert!(snap.available && snap.enabled && snap.profile.is_empty());
        assert_eq!(snap.curve.len(), 0);
        assert!(!parse(r#"{"enabled":false}"#).unwrap().enabled);
        assert!(parse("{\"profile\": \"half-writ").is_none());
    }
}
