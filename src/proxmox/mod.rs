//! The user's Proxmox server (no C-bar or reference equivalent; added at the
//! user's request; `docs/modules/proxmox.md`). The widget is
//! `modules/proxmox/`.
//!
//! - **Settings:** `~/.config/stickBar/proxmox.conf` ([`config`]): the API URL,
//!   a read-only API token, the server key's pin and its MAC. No file, no
//!   module.
//! - **What's polled:** `GET /api2/json/cluster/resources` (nodes, guests and
//!   storage in one call; [`resources`]) with `/usr/bin/curl`, through
//!   `util::spawn` on a named `proxmox-poll` thread, one poll at a time. The
//!   result comes back on a calloop channel to `App::proxmox_result`.
//! - **The token never shows in a process list:** curl gets it as a config
//!   line (`-K -`) on its stdin, not as an argument (every user can read
//!   `/proc/PID/cmdline`).
//! - **TLS:** Proxmox's certificate is self-signed, and lists the IP the
//!   server had when it was made. With a `pin`, curl skips the certificate
//!   checks (`-k`) but still refuses any server whose public key's hash
//!   isn't the pin (`--pinnedpubkey`). That check works at any IP and is
//!   stricter than a CA.
//! - **When:** 2 s after start, then every 30 s; every 3 s (each tick) while
//!   the dropdown is open or the server is waking. Never in test mode: test
//!   bars only poll on the tests' `refresh proxmox`.
//! - **Off vs. broken:** curl failing to connect or timing out means the
//!   server is off. Any other failure (a refused token, a pin mismatch, bad
//!   JSON) means it's on but something is wrong, and the dropdown says what.
//! - **Waking** ([`wol`]): the Wake chip sends the magic packet; the box shows
//!   "waking" until the API answers, or for at most 5 minutes.
//! - **Shutting down:** the dropdown's Shut down chip (armed by a first
//!   click, sent by a second) POSTs `command=shutdown` to the node, on a named
//!   `proxmox-power` thread. Proxmox stops every guest, then powers off. The
//!   box shows "shutting down" until a poll finds nothing answering, or for
//!   at most 5 minutes. The token needs `Sys.PowerMgmt` on the node for this
//!   (and only this). **A test bar only ever sends it to a fake curl** (the
//!   power menu's rule: a test must never power anything off).

pub(crate) mod config;
pub(crate) mod resources;
pub(crate) mod wol;

use std::time::{Duration, Instant};

use calloop::{
    LoopHandle, channel,
    timer::{TimeoutAction, Timer},
};

use self::{config::Config, resources::Cluster};
use crate::{util::spawn::run_capture_with_stdin, wayland::App};

/// curl. Absolute: `posix_spawn` doesn't search the PATH.
const CURL: &str = "/usr/bin/curl";
/// Replaces [`CURL`] (an absolute path), so tests run a fake.
const CURL_VAR: &str = "STICKBAR_TEST_PROXMOX_CURL";
/// The API call each poll makes, after the configured base URL.
const RESOURCES_PATH: &str = "/api2/json/cluster/resources";
/// The shutdown call: `POST` here, after the base URL, `{node}` filled in.
const NODE_STATUS_PATH: &str = "/api2/json/nodes/{node}/status";
/// The first poll, after start.
const FIRST_POLL: Duration = Duration::from_secs(2);
/// The timer's period. Each tick polls if one is due ([`Proxmox::tick`]).
const TICK: Duration = Duration::from_secs(3);
/// Between polls while nobody's looking (dropdown closed, not waking).
const INTERVAL: Duration = Duration::from_secs(30);
/// How long "waking" lasts without an answer before going back to "off".
/// A server takes 1-2 min to boot; past this, the packet was lost.
const WAKE_TIMEOUT: Duration = Duration::from_mins(5);
/// How long "shutting down" lasts while it still answers before going back
/// to what the polls say. Stopping every guest takes a minute or two; past
/// this, a guest refused to stop and Proxmox gave up on the shutdown.
const SHUTDOWN_TIMEOUT: Duration = Duration::from_mins(5);
/// curl's `--connect-timeout`, seconds. On the LAN a live server answers in
/// milliseconds; a powered-off one never does.
const CONNECT_TIMEOUT_S: &[u8] = b"2";
/// curl's `--max-time`, seconds, for the whole request.
const MAX_TIME_S: &[u8] = b"5";

/// curl's exit codes this module tells apart (`man curl`, "EXIT CODES").
mod curl_exit {
    /// Couldn't resolve the host name.
    pub(super) const RESOLVE: i32 = 6;
    /// Couldn't connect (refused, no route: the server is off or booting).
    pub(super) const CONNECT: i32 = 7;
    /// Timed out (a powered-off server never answers at all).
    pub(super) const TIMEOUT: i32 = 28;
    /// The TLS handshake failed.
    pub(super) const TLS: i32 = 35;
    /// The certificate didn't verify (no `pin` set, self-signed cert).
    pub(super) const CERT: i32 = 60;
    /// The server's public key isn't the pinned one.
    pub(super) const PIN: i32 = 90;
}

/// What a worker thread sends back to the main loop.
#[derive(Debug)]
pub(crate) enum Msg {
    /// A poll finished.
    Poll(PollResult),
    /// The shutdown request finished: sent, or why not.
    Shutdown(Result<(), String>),
}

/// What one poll found.
#[derive(Debug)]
pub(crate) enum PollResult {
    /// The API answered.
    Up(Cluster),
    /// Nothing answered: the server is off (or booting).
    Unreachable,
    /// Something answered, but not usefully; why.
    Failed(String),
}

/// What the server is doing, as the box and dropdown show it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Phase {
    /// No poll has finished yet.
    Unknown,
    /// Its API answers.
    Up,
    /// Nothing answers.
    Off,
    /// A magic packet was sent and it hasn't answered yet.
    Waking,
    /// A shutdown was accepted and it still answers (stopping guests).
    ShuttingDown,
    /// It answers, but with an error (`last_error`).
    Error,
}

/// The last poll's outcome, apart from waking.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
enum Reach {
    /// No poll has finished yet.
    #[default]
    Unknown,
    /// The API answered.
    Up,
    /// Nothing answered.
    Off,
    /// Something answered with an error.
    Error,
}

/// The server, as the last poll found it.
#[derive(Debug, Default)]
pub(crate) struct Proxmox {
    /// The settings; `None` = no settings file, so no module (no box).
    pub(crate) config: Option<Config>,
    /// The last answer's nodes, guests and storage: kept through an error,
    /// dropped once the server is off.
    pub(crate) cluster: Option<Cluster>,
    /// The last poll's outcome.
    reach: Reach,
    /// Why the last poll failed, while [`Phase::Error`].
    pub(crate) last_error: Option<String>,
    /// When the Wake chip was pressed, while waking.
    pub(crate) waking_since: Option<Instant>,
    /// When Proxmox accepted the shutdown, while shutting down.
    pub(crate) shutdown_since: Option<Instant>,
    /// A shutdown request is on its way (so a second one isn't sent).
    shutdown_sending: bool,
    /// Why the last Wake or Shut down failed, shown as the dropdown's red
    /// line. It isn't a server state, so the box stays as it is. Cleared by
    /// the next successful action, and when the dropdown opens.
    pub(crate) action_error: Option<String>,
    /// A poll is running now.
    pub(crate) polling: bool,
    /// When the last poll started.
    last_poll: Option<Instant>,
    /// When the API last answered.
    pub(crate) last_ok: Option<Instant>,
    /// Test mode: the magic packet only goes to the test's address.
    test_mode: bool,
    /// Where finished polls and shutdowns report (set by [`start`]).
    results: Option<channel::Sender<Msg>>,
}

impl Proxmox {
    /// Whether the module is on (it has settings): no box otherwise.
    pub(crate) const fn enabled(&self) -> bool {
        self.config.is_some()
    }

    /// What the server is doing.
    pub(crate) const fn phase(&self) -> Phase {
        if self.shutdown_since.is_some() {
            return Phase::ShuttingDown;
        }
        if self.waking_since.is_some() {
            return Phase::Waking;
        }
        match self.reach {
            Reach::Unknown => Phase::Unknown,
            Reach::Up => Phase::Up,
            Reach::Off => Phase::Off,
            Reach::Error => Phase::Error,
        }
    }

    /// The timer's tick (every [`TICK`]): ends a "waking" or "shutting down"
    /// that took too long, and polls if one is due. `fast` (the dropdown is
    /// open) polls every tick, as do waking and shutting down. Returns
    /// whether what's shown changed.
    pub(crate) fn tick(&mut self, fast: bool) -> bool {
        let mut changed = false;
        if self
            .waking_since
            .is_some_and(|since| since.elapsed() >= WAKE_TIMEOUT)
        {
            self.waking_since = None;
            changed = true;
        }
        if self
            .shutdown_since
            .is_some_and(|since| since.elapsed() >= SHUTDOWN_TIMEOUT)
        {
            self.shutdown_since = None;
            changed = true;
        }
        let due = fast
            || self.waking_since.is_some()
            || self.shutdown_since.is_some()
            || self.last_poll.is_none_or(|t| t.elapsed() >= INTERVAL);
        if due {
            self.poll();
        }
        changed
    }

    /// Starts a poll in the background, unless one is running. The result
    /// arrives at `App::proxmox_result`.
    pub(crate) fn poll(&mut self) {
        if self.polling {
            return;
        }
        let (Some(config), Some(results)) = (self.config.clone(), self.results.clone()) else {
            return; // no settings, or `start` failed: polls are off for this run
        };
        let started = std::thread::Builder::new()
            .name("proxmox-poll".to_owned())
            .spawn(move || {
                if results.send(Msg::Poll(fetch(&config))).is_err() {
                    eprintln!("stickBar: proxmox: the main loop stopped listening");
                }
            });
        match started {
            // Detached: it ends by itself when curl does (--max-time).
            Ok(_detached) => {
                self.polling = true;
                self.last_poll = Some(Instant::now());
            }
            Err(err) => eprintln!("stickBar: proxmox: couldn't start a poll thread: {err}"),
        }
    }

    /// Takes in a finished poll.
    pub(crate) fn finish(&mut self, result: PollResult) {
        self.polling = false;
        match result {
            PollResult::Up(cluster) => {
                self.cluster = Some(cluster);
                self.reach = Reach::Up;
                self.last_error = None;
                self.last_ok = Some(Instant::now());
                self.waking_since = None; // it's awake
            }
            PollResult::Unreachable => {
                // Off: last time's guests aren't running any more. A waking
                // server stays "waking" (it's still booting); a shutting-down
                // one has finished.
                self.cluster = None;
                self.reach = Reach::Off;
                self.last_error = None;
                self.shutdown_since = None;
            }
            PollResult::Failed(error) => {
                // Something answered, so it's awake; keep the last figures
                // (an expired token shouldn't blank the guest list). A
                // shutdown carries on: the API may fail as it stops.
                if self.last_error.as_ref() != Some(&error) {
                    eprintln!("stickBar: proxmox: {error}");
                }
                self.reach = Reach::Error;
                self.last_error = Some(error);
                self.waking_since = None;
            }
        }
    }

    /// Sends the magic packet and starts "waking" (polling every tick until
    /// it answers). Errors are the packet's (the bar logs them).
    pub(crate) fn wake(&mut self) -> Result<(), String> {
        let mac = self.config.as_ref().ok_or("no settings")?.mac;
        if let Err(err) = wol::send(mac, self.test_mode) {
            self.action_error = Some(format!("Wake failed: {err}"));
            return Err(err);
        }
        self.waking_since = Some(Instant::now());
        self.shutdown_since = None;
        self.action_error = None;
        self.poll();
        Ok(())
    }

    /// Asks the node to shut down, in the background (the Shut down chip's
    /// second click). The answer arrives at `App::proxmox_shutdown_result`.
    /// Errors are why it wasn't even sent; they're shown too.
    pub(crate) fn shutdown(&mut self) -> Result<(), String> {
        let sent = self.send_shutdown();
        if let Err(err) = &sent {
            self.action_error = Some(format!("Shut down failed: {err}"));
        }
        sent
    }

    /// [`Self::shutdown`]'s work.
    fn send_shutdown(&mut self) -> Result<(), String> {
        if self.shutdown_sending {
            return Ok(()); // one is on its way already
        }
        // NEVER from a test bar to the real curl: a test must never power
        // off the user's server (the power menu's rule).
        if self.test_mode && !std::env::var(CURL_VAR).is_ok_and(|v| !v.is_empty()) {
            return Err(format!("test mode without {CURL_VAR}: not sent"));
        }
        let config = self.config.clone().ok_or("no settings")?;
        let node = self
            .cluster
            .as_ref()
            .and_then(|c| c.nodes.first())
            .map(|n| n.name.clone())
            .ok_or("no node known yet")?;
        // It goes into the URL: only a valid node name (letters, digits, `-`).
        if node.is_empty() || !node.chars().all(|c| c.is_ascii_alphanumeric() || c == '-') {
            return Err(format!("odd node name `{node}`"));
        }
        let results = self.results.clone().ok_or("no main loop channel")?;
        let started = std::thread::Builder::new()
            .name("proxmox-power".to_owned())
            .spawn(move || {
                if results
                    .send(Msg::Shutdown(post_shutdown(&config, &node)))
                    .is_err()
                {
                    eprintln!("stickBar: proxmox: the main loop stopped listening");
                }
            });
        match started {
            // Detached: it ends by itself when curl does (--max-time).
            Ok(_detached) => {
                self.shutdown_sending = true;
                Ok(())
            }
            Err(err) => Err(format!("couldn't start a thread: {err}")),
        }
    }

    /// The shutdown request finished: on success "shutting down" starts (and
    /// polls run every tick until it's off).
    pub(crate) fn shutdown_finished(&mut self, result: Result<(), String>) {
        self.shutdown_sending = false;
        match result {
            Ok(()) => {
                self.shutdown_since = Some(Instant::now());
                self.waking_since = None;
                self.action_error = None;
                self.poll();
            }
            Err(err) => {
                eprintln!("stickBar: proxmox: shutdown failed: {err}");
                self.action_error = Some(format!("Shut down failed: {err}"));
            }
        }
    }
}

/// The command for curl: [`CURL_VAR`]'s path, or [`CURL`].
fn curl_path() -> String {
    std::env::var(CURL_VAR)
        .ok()
        .filter(|v| !v.is_empty())
        .unwrap_or_else(|| CURL.to_owned())
}

/// Runs one poll: curl, then [`interpret`]. Blocks for up to `--max-time`:
/// call it from the poll thread.
fn fetch(config: &Config) -> PollResult {
    let url = format!("{}{RESOURCES_PATH}", config.url);
    match run_curl(config, &url, &[]) {
        Ok((code, out)) => interpret(code, &out),
        Err(err) => PollResult::Failed(format!("couldn't run curl: {err}")),
    }
}

/// Asks `node` to shut down: `POST .../nodes/{node}/status` with
/// `command=shutdown`. Blocks for up to `--max-time`: call it from a worker
/// thread. Proxmox answers at once (the shutdown runs as a task there).
fn post_shutdown(config: &Config, node: &str) -> Result<(), String> {
    let url = format!("{}{}", config.url, NODE_STATUS_PATH.replace("{node}", node));
    let post: [&[u8]; 4] = [b"--request", b"POST", b"--data", b"command=shutdown"];
    let (code, out) =
        run_curl(config, &url, &post).map_err(|e| format!("couldn't run curl: {e}"))?;
    interpret_shutdown(code, &out)
}

/// Runs curl against `url` with the shared options (timeouts, the pin, the
/// HTTP status after the body, the token on stdin) plus `extra`. Returns
/// (exit code, output).
fn run_curl(config: &Config, url: &str, extra: &[&[u8]]) -> Result<(i32, String), String> {
    let path = curl_path();
    let mut args: Vec<&[u8]> = vec![
        path.as_bytes(),
        b"--silent",
        b"--connect-timeout",
        CONNECT_TIMEOUT_S,
        b"--max-time",
        MAX_TIME_S,
        // After the body, a newline and the HTTP status (curl expands the \n).
        b"--write-out",
        b"\\n%{http_code}",
        // The Authorization header comes as a curl config line on stdin.
        b"--config",
        b"-",
    ];
    if let Some(pin) = &config.pin {
        args.extend_from_slice(&[b"--insecure", b"--pinnedpubkey", pin.as_bytes()]);
    }
    args.extend_from_slice(extra);
    args.push(url.as_bytes());
    // `config::parse` allowed only characters that need no escaping here.
    let header = format!("header = \"Authorization: PVEAPIToken={}\"\n", config.token);
    run_capture_with_stdin(&path, &args, Some(header.as_bytes()))
}

/// What a failed curl exit code means: `Unreachable` (off) or `Failed`
/// with why; `None` for exit 0 (the HTTP status then decides).
fn curl_error(code: i32) -> Option<PollResult> {
    let failed = |why: &str| Some(PollResult::Failed(why.to_owned()));
    match code {
        0 => None,
        curl_exit::CONNECT | curl_exit::TIMEOUT => Some(PollResult::Unreachable),
        curl_exit::RESOLVE => failed("can't resolve the host name"),
        curl_exit::TLS => failed("the TLS handshake failed"),
        curl_exit::CERT => failed("the certificate isn't trusted (set `pin` in proxmox.conf)"),
        curl_exit::PIN => failed("the server's key doesn't match `pin` (new certificate?)"),
        -1 => failed("curl was killed"),
        _ => Some(PollResult::Failed(format!("curl failed (exit {code})"))),
    }
}

/// Turns curl's exit code and output (the body, a newline, the HTTP status)
/// into a [`PollResult`].
fn interpret(code: i32, out: &str) -> PollResult {
    if let Some(error) = curl_error(code) {
        return error;
    }
    let (body, status) = out.rsplit_once('\n').unwrap_or(("", out));
    match status.trim() {
        "200" => match resources::parse(body) {
            Ok(cluster) => PollResult::Up(cluster),
            Err(err) => PollResult::Failed(err),
        },
        "401" => PollResult::Failed("the API token was refused (HTTP 401)".to_owned()),
        "403" => PollResult::Failed("the API token lacks permission (HTTP 403)".to_owned()),
        other => PollResult::Failed(format!("HTTP {other}")),
    }
}

/// Turns the shutdown request's curl exit code and output into sent or why
/// not. 403 is the likely one: the read-only token wasn't given
/// `Sys.PowerMgmt` on the node.
fn interpret_shutdown(code: i32, out: &str) -> Result<(), String> {
    match curl_error(code) {
        None | Some(PollResult::Up(_)) => {}
        Some(PollResult::Unreachable) => return Err("the server didn't answer".to_owned()),
        Some(PollResult::Failed(why)) => return Err(why),
    }
    let status = out.rsplit_once('\n').map_or(out, |(_body, status)| status);
    match status.trim() {
        "200" => Ok(()),
        "403" => Err("the token lacks Sys.PowerMgmt on the node (HTTP 403)".to_owned()),
        "401" => Err("the API token was refused (HTTP 401)".to_owned()),
        other => Err(format!("HTTP {other}")),
    }
}

/// Joins the module to the main loop: reads the settings and, if there are
/// any, the channel results come back on and (outside test mode) the timer.
pub(crate) fn start(handle: &LoopHandle<'static, App>, proxmox: &mut Proxmox, test_mode: bool) {
    proxmox.test_mode = test_mode;
    match config::load(test_mode) {
        Ok(Some(config)) => proxmox.config = Some(config),
        Ok(None) => return, // no settings file: no module
        Err(err) => {
            eprintln!("stickBar: proxmox: {err} -- the Proxmox box is off");
            return;
        }
    }
    let (results, source) = channel::channel::<Msg>();
    let inserted = handle.insert_source(source, |event, (), app| match event {
        channel::Event::Msg(Msg::Poll(result)) => app.proxmox_result(result),
        channel::Event::Msg(Msg::Shutdown(result)) => app.proxmox_shutdown_result(result),
        channel::Event::Closed => {}
    });
    if let Err(err) = inserted {
        eprintln!(
            "stickBar: proxmox: couldn't listen for results ({}) -- polls disabled",
            err.error
        );
        return;
    }
    proxmox.results = Some(results);
    if test_mode {
        return; // only the tests' `refresh proxmox` polls
    }
    let timer = handle.insert_source(Timer::from_duration(FIRST_POLL), |_deadline, (), app| {
        app.proxmox_tick();
        TimeoutAction::ToDuration(TICK)
    });
    if let Err(err) = timer {
        eprintln!("stickBar: proxmox: couldn't start the timer: {}", err.error);
    }
}

#[cfg(test)]
mod tests {
    use super::{Phase, PollResult, Proxmox, interpret, interpret_shutdown, resources::Cluster};

    const BODY: &str = r#"{"data":[{"type":"node","node":"pve","status":"online"}]}"#;

    #[test]
    fn curl_outcomes_map_to_states() {
        assert!(matches!(interpret(7, ""), PollResult::Unreachable));
        assert!(matches!(interpret(28, "\n000"), PollResult::Unreachable));
        assert!(matches!(interpret(90, ""), PollResult::Failed(e) if e.contains("pin")));
        assert!(matches!(
            interpret(0, &format!("{BODY}\n200")),
            PollResult::Up(c) if c.nodes.len() == 1
        ));
        assert!(matches!(
            interpret(0, "{\"data\":null}\n401"),
            PollResult::Failed(e) if e.contains("401")
        ));
        assert!(matches!(interpret(0, "x\n500"), PollResult::Failed(e) if e == "HTTP 500"));
    }

    #[test]
    fn waking_lasts_until_it_answers_and_errors_keep_the_figures() {
        let mut proxmox = Proxmox::default();
        assert_eq!(proxmox.phase(), Phase::Unknown);
        proxmox.finish(PollResult::Unreachable);
        assert_eq!(proxmox.phase(), Phase::Off);
        proxmox.waking_since = Some(std::time::Instant::now());
        proxmox.finish(PollResult::Unreachable);
        assert_eq!(proxmox.phase(), Phase::Waking);
        proxmox.finish(PollResult::Up(Cluster::default()));
        assert_eq!(proxmox.phase(), Phase::Up);
        proxmox.finish(PollResult::Failed("HTTP 500".to_owned()));
        assert_eq!(proxmox.phase(), Phase::Error);
        assert!(proxmox.cluster.is_some());
        proxmox.finish(PollResult::Unreachable);
        assert!(proxmox.cluster.is_none());
    }

    #[test]
    fn shutting_down_lasts_until_nothing_answers() {
        let mut proxmox = Proxmox::default();
        proxmox.finish(PollResult::Up(Cluster::default()));
        proxmox.shutdown_finished(Ok(()));
        assert_eq!(proxmox.phase(), Phase::ShuttingDown);
        // Still answering (stopping guests), or failing as the API stops.
        proxmox.finish(PollResult::Up(Cluster::default()));
        proxmox.finish(PollResult::Failed("HTTP 595".to_owned()));
        assert_eq!(proxmox.phase(), Phase::ShuttingDown);
        proxmox.finish(PollResult::Unreachable);
        assert_eq!(proxmox.phase(), Phase::Off);
        // A failed request doesn't start it, and is shown.
        proxmox.shutdown_finished(Err("HTTP 403".to_owned()));
        assert_eq!(proxmox.phase(), Phase::Off);
        assert!(proxmox.action_error.is_some());
    }

    #[test]
    fn shutdown_answers_map_to_sent_or_why() {
        assert_eq!(
            interpret_shutdown(0, "{\"data\":\"UPID:pve\"}\n200"),
            Ok(())
        );
        assert!(matches!(interpret_shutdown(0, "{}\n403"), Err(e) if e.contains("Sys.PowerMgmt")));
        assert!(matches!(interpret_shutdown(7, ""), Err(e) if e.contains("didn't answer")));
        assert!(matches!(interpret_shutdown(90, ""), Err(e) if e.contains("pin")));
    }
}
