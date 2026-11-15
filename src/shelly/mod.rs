//! The user's Shelly smart plugs (no C-bar or reference equivalent; added at
//! the user's request from their own `shelly.py`; `docs/modules/shelly.md`).
//! They're a card in the media dropdown (`modules/media/shelly_section.rs`),
//! with nothing in the bar itself.
//!
//! - **Settings:** `~/.config/stickBar/shelly.conf` ([`config`]): one
//!   `name = IP` per plug. No file, no card.
//! - **The API:** Gen2+ RPC over plain HTTP ([`http`]). The user's plugs are
//!   Gen3; Gen2 and Gen4 speak the same API, Gen1 doesn't (it answers 404 on
//!   `/rpc/…`, shown as "not Gen2+"). Reading a plug is
//!   `/rpc/Switch.GetStatus?id=0`, whose answer carries `output` (on/off) and
//!   `apower` (watts) together, so the watts cost no extra request. Setting
//!   one is `/rpc/Switch.Set?id=0&on=…`, then a `GetStatus` for the state and
//!   watts it ended up with.
//! - **Threads:** each batch (a poll of every plug, or a set of one or all)
//!   runs on its own named `shelly` thread, which asks every plug at once
//!   from scoped `shelly-req` threads, so one unplugged plug (1.5 s timeout)
//!   doesn't delay the others. The answers come back together on a calloop
//!   channel to `App::shelly_result`.
//! - **When:** only while the media dropdown is open: once as it opens, then
//!   every [`POLL_EVERY_TICKS`] clock ticks (2 s), never two polls at once.
//!   With the dropdown closed the plugs are never asked anything. Test bars
//!   follow the same rule; they can only ever reach the tests' fake plugs
//!   ([`config`]'s test rule).
//! - **Stale answers are dropped:** each plug counts the sets sent to it
//!   ([`Plug::seq`]). A poll that started before a click, and answers after
//!   it, would otherwise flash the old state back for 2 s.

pub(crate) mod config;
pub(crate) mod http;

use std::net::SocketAddr;

use calloop::{LoopHandle, channel};
use serde_json::Value;

use self::{
    config::PlugConfig,
    http::{Failure, get},
};
use crate::wayland::App;

/// Polls run on every this-many clock ticks (1 s each) while the dropdown is open.
const POLL_EVERY_TICKS: u32 = 2;
/// Reads a plug's switch: its state and live power.
const STATUS_PATH: &str = "/rpc/Switch.GetStatus?id=0";

/// What a plug is doing, as the card shows it.
#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) enum PlugState {
    /// Not asked yet (the dropdown hasn't been open since the bar started).
    #[default]
    Unknown,
    /// Switched on.
    On,
    /// Switched off.
    Off,
    /// Nothing answered: unplugged, or away from the LAN.
    Offline,
    /// Something answered, but not usefully; a short reason for the row.
    Error(String),
}

/// One plug: its settings, and what it last said.
#[derive(Clone, Debug)]
pub(crate) struct Plug {
    /// Its name, as the settings file gives it.
    pub(crate) name: String,
    /// Where it answers.
    addr: SocketAddr,
    /// What it's doing.
    pub(crate) state: PlugState,
    /// Its power draw, W, when it last reported one.
    pub(crate) watts: Option<f64>,
    /// A set was sent and hasn't answered yet (the row shows `…`).
    pub(crate) pending: bool,
    /// How many sets have been sent to it. An answer to a request sent
    /// before the latest set is stale and dropped.
    seq: u64,
}

/// What one request does.
#[derive(Clone, Copy, Debug)]
enum Action {
    /// Read the state and watts.
    Status,
    /// Switch on (`true`) or off, then read them.
    Set(bool),
}

/// One plug's answer.
#[derive(Debug)]
pub(crate) struct Reply {
    /// Which plug, as an index into [`Shelly::plugs`].
    index: usize,
    /// Its [`Plug::seq`] when the request was sent.
    seq: u64,
    /// What it said.
    state: PlugState,
    /// Its power, if it said.
    watts: Option<f64>,
}

/// A finished batch, as the worker thread sends it back.
#[derive(Debug)]
pub(crate) struct Batch {
    /// A poll (`true`) or a set.
    poll: bool,
    /// One per plug asked.
    replies: Vec<Reply>,
}

/// The plugs, and the polling state.
#[derive(Debug, Default)]
pub(crate) struct Shelly {
    /// Every configured plug, in the file's order. Empty: no card.
    pub(crate) plugs: Vec<Plug>,
    /// A poll is running now.
    pub(crate) polling: bool,
    /// Clock ticks since the last poll, while the dropdown is open.
    ticks: u32,
    /// Where finished batches report (set by [`start`]).
    results: Option<channel::Sender<Batch>>,
}

impl Shelly {
    /// Whether there's a card (a settings file listed plugs).
    pub(crate) fn enabled(&self) -> bool {
        !self.plugs.is_empty()
    }

    /// How many plugs are on.
    pub(crate) fn on_count(&self) -> usize {
        self.plugs
            .iter()
            .filter(|p| p.state == PlugState::On)
            .count()
    }

    /// The watts of every plug that's on, added up.
    pub(crate) fn total_watts(&self) -> f64 {
        self.plugs
            .iter()
            .filter(|p| p.state == PlugState::On)
            .filter_map(|p| p.watts)
            .sum()
    }

    /// The media dropdown's 1 s tick while it's open: polls every
    /// [`POLL_EVERY_TICKS`]th tick.
    pub(crate) fn tick(&mut self) {
        self.ticks += 1;
        if self.ticks >= POLL_EVERY_TICKS {
            self.poll();
        }
    }

    /// Asks every plug for its state, in the background, unless a poll is
    /// already running. The answers arrive at `App::shelly_result`.
    pub(crate) fn poll(&mut self) {
        if self.polling || !self.enabled() {
            return;
        }
        let jobs = self
            .plugs
            .iter()
            .enumerate()
            .map(|(index, p)| (index, p.addr, p.seq, Action::Status))
            .collect();
        if self.run(true, jobs) {
            self.polling = true;
            self.ticks = 0;
        }
    }

    /// Switches plug `index` on or off, or every plug with `None`. The rows
    /// show `…` until the answers arrive.
    pub(crate) fn set(&mut self, index: Option<usize>, on: bool) {
        let mut jobs = Vec::new();
        for (i, plug) in self.plugs.iter_mut().enumerate() {
            if index.is_none_or(|wanted| wanted == i) {
                plug.seq += 1;
                plug.pending = true;
                jobs.push((i, plug.addr, plug.seq, Action::Set(on)));
            }
        }
        if !jobs.is_empty() && !self.run(false, jobs) {
            // Never sent: nothing will clear the `…`.
            for plug in &mut self.plugs {
                plug.pending = false;
            }
        }
    }

    /// Starts a batch on a `shelly` thread. Returns whether it started.
    fn run(&self, poll: bool, jobs: Vec<(usize, SocketAddr, u64, Action)>) -> bool {
        let Some(results) = self.results.clone() else {
            return false; // `start` found no settings or failed
        };
        let started = std::thread::Builder::new()
            .name("shelly".to_owned())
            .spawn(move || {
                let batch = Batch {
                    poll,
                    replies: ask_all(jobs),
                };
                if results.send(batch).is_err() {
                    eprintln!("stickBar: shelly: the main loop stopped listening");
                }
            });
        match started {
            // Detached: it ends by itself once every plug answered or timed out.
            Ok(_detached) => true,
            Err(err) => {
                eprintln!("stickBar: shelly: couldn't start a request thread: {err}");
                false
            }
        }
    }

    /// Takes in a finished batch. Answers sent before a later set are dropped.
    pub(crate) fn finish(&mut self, batch: Batch) {
        if batch.poll {
            self.polling = false;
        }
        for reply in batch.replies {
            let Some(plug) = self.plugs.get_mut(reply.index) else {
                continue;
            };
            if reply.seq != plug.seq {
                continue; // a set went out after this was asked
            }
            if let PlugState::Error(ref why) = reply.state
                && plug.state != reply.state
            {
                eprintln!("stickBar: shelly: {}: {why}", plug.name);
            }
            plug.state = reply.state;
            plug.watts = reply.watts;
            if !batch.poll {
                plug.pending = false;
            }
        }
    }
}

/// Runs every job at once, each on a scoped `shelly-req` thread, and returns
/// their answers. A thread that can't start answers as an error.
fn ask_all(jobs: Vec<(usize, SocketAddr, u64, Action)>) -> Vec<Reply> {
    std::thread::scope(|scope| {
        // Every thread starts before any is joined: that's what makes the
        // plugs answer in parallel (a lazy map-then-join would ask them one
        // after another).
        let mut handles = Vec::with_capacity(jobs.len());
        for (index, addr, seq, action) in jobs {
            let handle = std::thread::Builder::new()
                .name("shelly-req".to_owned())
                .spawn_scoped(scope, move || ask(addr, action));
            handles.push((index, seq, handle));
        }
        handles
            .into_iter()
            .map(|(index, seq, handle)| {
                let (state, watts) = match handle {
                    Ok(handle) => handle.join().unwrap_or_else(|_panic| {
                        (PlugState::Error("request thread crashed".to_owned()), None)
                    }),
                    Err(err) => (PlugState::Error(format!("no thread: {err}")), None),
                };
                Reply {
                    index,
                    seq,
                    state,
                    watts,
                }
            })
            .collect()
    })
}

/// One plug, one action: what it says it's doing afterwards.
fn ask(addr: SocketAddr, action: Action) -> (PlugState, Option<f64>) {
    if let Action::Set(on) = action {
        let path = format!("/rpc/Switch.Set?id=0&on={on}");
        match get(addr, &path) {
            Ok((200, _)) => {}
            Ok((code, _)) => return (http_error(code), None),
            Err(failure) => return (failed(failure), None),
        }
    }
    match get(addr, STATUS_PATH) {
        Ok((200, body)) => interpret_status(&body),
        Ok((code, _)) => (http_error(code), None),
        Err(failure) => (failed(failure), None),
    }
}

/// A non-200 answer, as the row says it.
fn http_error(code: u16) -> PlugState {
    PlugState::Error(match code {
        // Gen2+ answers 401 to everything once a password is set; digest
        // auth isn't implemented (docs/modules/shelly.md).
        401 => "password set".to_owned(),
        // A Gen1 plug has no `/rpc`.
        404 => "not Gen2+".to_owned(),
        _ => format!("HTTP {code}"),
    })
}

/// No HTTP answer, as the row says it.
fn failed(failure: Failure) -> PlugState {
    match failure {
        Failure::NoAnswer => PlugState::Offline,
        Failure::Garbled(why) => PlugState::Error(why),
    }
}

/// A `Switch.GetStatus` answer: `output` is the state, `apower` the watts
/// (absent on a switch without a power meter).
fn interpret_status(body: &str) -> (PlugState, Option<f64>) {
    let Ok(json) = serde_json::from_str::<Value>(body) else {
        return (PlugState::Error("bad JSON".to_owned()), None);
    };
    let watts = json.get("apower").and_then(Value::as_f64);
    match json.get("output").and_then(Value::as_bool) {
        Some(true) => (PlugState::On, watts),
        Some(false) => (PlugState::Off, watts),
        None => (PlugState::Error("no `output`".to_owned()), None),
    }
}

/// Joins the plugs to the main loop: reads the settings and, if there are
/// any, the channel batches come back on. Nothing is polled until the media
/// dropdown opens.
pub(crate) fn start(handle: &LoopHandle<'static, App>, shelly: &mut Shelly, test_mode: bool) {
    let configs = match config::load(test_mode) {
        Ok(Some(configs)) => configs,
        Ok(None) => return, // no settings file: no card
        Err(err) => {
            eprintln!("stickBar: shelly: {err} -- the smart-plug card is off");
            return;
        }
    };
    let (results, source) = channel::channel::<Batch>();
    let inserted = handle.insert_source(source, |event, (), app| {
        if let channel::Event::Msg(batch) = event {
            app.shelly_result(batch);
        }
    });
    if let Err(err) = inserted {
        eprintln!(
            "stickBar: shelly: couldn't listen for answers ({}) -- the smart-plug card is off",
            err.error
        );
        return;
    }
    shelly.plugs = configs
        .into_iter()
        .map(|PlugConfig { name, addr }| Plug {
            name,
            addr,
            state: PlugState::Unknown,
            watts: None,
            pending: false,
            seq: 0,
        })
        .collect();
    shelly.results = Some(results);
}

#[cfg(test)]
mod tests {
    use std::net::SocketAddr;

    use super::{Batch, Plug, PlugState, Reply, Shelly, http_error, interpret_status};

    #[test]
    fn reads_state_and_watts() {
        let body = r#"{"id":0,"output":true,"apower":42.5,"voltage":231.2}"#;
        assert_eq!(interpret_status(body), (PlugState::On, Some(42.5)));
        assert_eq!(
            interpret_status(r#"{"output":false,"apower":0}"#),
            (PlugState::Off, Some(0.0))
        );
        assert_eq!(interpret_status(r#"{"output":false}"#).1, None);
        assert!(matches!(interpret_status("{}").0, PlugState::Error(_)));
        assert!(matches!(interpret_status("<html>").0, PlugState::Error(_)));
        assert_eq!(http_error(401), PlugState::Error("password set".to_owned()));
        assert_eq!(http_error(404), PlugState::Error("not Gen2+".to_owned()));
    }

    /// A plug with no answer yet.
    fn plug(name: &str) -> Plug {
        Plug {
            name: name.to_owned(),
            addr: SocketAddr::from(([127, 0, 0, 1], 80)),
            state: PlugState::Unknown,
            watts: None,
            pending: false,
            seq: 0,
        }
    }

    /// One reply.
    const fn reply(index: usize, seq: u64, state: PlugState) -> Reply {
        Reply {
            index,
            seq,
            state,
            watts: None,
        }
    }

    #[test]
    fn a_poll_older_than_a_set_is_dropped() {
        let mut shelly = Shelly {
            plugs: vec![plug("a"), plug("b")],
            ..Shelly::default()
        };
        // No channel: nothing is sent, and the pending marks are undone.
        shelly.set(Some(0), true);
        assert!(!shelly.plugs[0].pending);
        assert_eq!(shelly.plugs[0].seq, 1);
        // A poll asked before the set (seq 0) answers after it: dropped for
        // plug 0, kept for plug 1.
        shelly.finish(Batch {
            poll: true,
            replies: vec![reply(0, 0, PlugState::Off), reply(1, 0, PlugState::On)],
        });
        assert_eq!(shelly.plugs[0].state, PlugState::Unknown);
        assert_eq!(shelly.plugs[1].state, PlugState::On);
        // The set's own answer lands, and clears the `…`.
        shelly.plugs[0].pending = true;
        shelly.finish(Batch {
            poll: false,
            replies: vec![reply(0, 1, PlugState::On)],
        });
        assert_eq!(shelly.plugs[0].state, PlugState::On);
        assert!(!shelly.plugs[0].pending);
        assert_eq!(shelly.on_count(), 2);
    }
}
