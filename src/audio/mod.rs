//! Volume and mic data, from the PulseAudio-protocol server (`pipewire-pulse`
//! on this machine). The C bar's `audio/pulse_client.c`; why libpulse and
//! not native `PipeWire`: `docs/modules/audio.md`.
//!
//! **Threads, the Rust way:** libpulse needs a loop of its own, so it gets a
//! thread (`pulse.rs`) that *owns* the libpulse mainloop, the connection and
//! the device cache. Nothing is shared, so nothing needs a lock (the C bar
//! shares its cache under libpulse's mutex instead). The two threads talk
//! through channels only:
//!
//! - pulse → main: after anything changed, a full [`AudioState`] snapshot
//!   (a few devices and streams, cheap to copy) on a calloop channel, which
//!   wakes the main loop like any other event source;
//! - main → pulse: a [`Command`] on an `mpsc` channel, plus one byte on a
//!   socket pair to wake the pulse thread's loop, which is blocked in `poll`.
//!
//! The main thread keeps the latest snapshot in the `Renderer` and draws from
//! it; [`Audio`] is its handle for sending commands.

pub(crate) mod device_popup;
pub(crate) mod mixer_popup;
mod pulse;
pub(crate) mod volume_style;

use std::{io::Write, os::unix::net::UnixStream, sync::mpsc, time::Duration};

use calloop::{LoopHandle, channel};

use crate::wayland::App;

/// Scrolling steps a volume by this much (the reference bar's `step: 0.05`).
pub(crate) const VOLUME_STEP: i32 = 5;
/// Volumes can go up to 200 % (the reference bar's `maxVolume: 2.0`).
pub(crate) const MAX_VOLUME: u32 = 200;
/// How long startup waits for the connection (the C bar blocks until it's
/// ready or failed; this bounds it in case the server hangs).
const CONNECT_TIMEOUT: Duration = Duration::from_secs(2);

/// Sinks (outputs) or sources (inputs).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Kind {
    /// An output device; the volume box's.
    Sink,
    /// An input device; the mic box's.
    Source,
}

/// One sink or source.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct Node {
    /// The server's index, used for set requests.
    pub(crate) index: u32,
    /// The stable name, matched against the default's name.
    pub(crate) name: String,
    /// The human-readable name, e.g. `Yeti X Analog Stereo`.
    pub(crate) description: String,
    /// Average channel volume, % of normal (can be over 100).
    pub(crate) volume: u32,
    /// Whether it's muted.
    pub(crate) muted: bool,
    /// Channel count (new volumes are set on this many channels).
    pub(crate) channels: u8,
    /// `running`, `idle`, `suspended` or "".
    pub(crate) state: &'static str,
    /// The `device.bus` property, upper-cased (`USB`, `PCI`), or "".
    pub(crate) bus: String,
    /// Sample rate in Hz, 0 if unknown.
    pub(crate) rate: u32,
    /// A sink's `.monitor` source (sources only).
    pub(crate) monitor: bool,
}

/// One app's playback stream (a `PulseAudio` "sink input").
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct Stream {
    /// The server's index, used for set requests.
    pub(crate) index: u32,
    /// `application.name`, else the stream's own name.
    pub(crate) name: String,
    /// Average channel volume, % of normal.
    pub(crate) volume: u32,
    /// Whether it's muted.
    pub(crate) muted: bool,
    /// Channel count.
    pub(crate) channels: u8,
    /// Whether it takes a volume at all (passthrough streams don't; their
    /// volume requests are skipped, as in C).
    pub(crate) has_volume: bool,
}

/// Everything known about the audio server: a snapshot from the pulse thread.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct AudioState {
    /// Every sink, in the order they appeared.
    pub(crate) sinks: Vec<Node>,
    /// Every source, monitors included.
    pub(crate) sources: Vec<Node>,
    /// Every playback stream.
    pub(crate) streams: Vec<Stream>,
    /// The default sink's name ("" until the server says).
    pub(crate) default_sink: String,
    /// The default source's name.
    pub(crate) default_source: String,
}

impl AudioState {
    /// The sinks or the sources.
    pub(crate) fn nodes(&self, kind: Kind) -> &[Node] {
        match kind {
            Kind::Sink => &self.sinks,
            Kind::Source => &self.sources,
        }
    }

    /// The app stream with server index `index`.
    pub(crate) fn stream(&self, index: u32) -> Option<&Stream> {
        self.streams.iter().find(|s| s.index == index)
    }

    /// The sink or source with server index `index`.
    pub(crate) fn node(&self, kind: Kind, index: u32) -> Option<&Node> {
        self.nodes(kind).iter().find(|n| n.index == index)
    }

    /// Whether `node` is the default of its kind.
    pub(crate) fn is_default(&self, kind: Kind, node: &Node) -> bool {
        self.default_node(kind)
            .is_some_and(|d| d.index == node.index)
    }

    /// The default sink or source, if known.
    pub(crate) fn default_node(&self, kind: Kind) -> Option<&Node> {
        let (nodes, name) = match kind {
            Kind::Sink => (&self.sinks, &self.default_sink),
            Kind::Source => (&self.sources, &self.default_source),
        };
        nodes
            .iter()
            .find(|node| !name.is_empty() && node.name == *name)
    }

    /// What the bar shows: (volume %, muted) of the default sink and source,
    /// (0, false) when unknown, as in C. The bars are redrawn only when this
    /// changes: most server events are other apps' streams or idle devices
    /// (in the C bar, 300 no-op events cost 660 ms of CPU before that rule).
    pub(crate) fn shown(&self) -> [(u32, bool); 2] {
        [Kind::Sink, Kind::Source].map(|kind| {
            self.default_node(kind)
                .map_or((0, false), |n| (n.volume, n.muted))
        })
    }
}

/// A request for the pulse thread. Volume changes are worked out there, from
/// its own up-to-date cache.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum Command {
    /// Step the default sink's or source's volume by `delta` % (clamped 0..=200).
    AdjustDefault {
        /// Which default.
        kind: Kind,
        /// The step, e.g. ±5.
        delta: i32,
    },
    /// Flip the default sink's or source's mute.
    ToggleDefaultMute(Kind),
    /// Set one device's volume to `percent` (clamped 0..=200): the device
    /// dropdown's meter.
    SetVolume {
        /// Sink or source.
        kind: Kind,
        /// The device's server index.
        index: u32,
        /// The new volume.
        percent: u32,
    },
    /// Step one device's volume by `delta` %: scrolling on its card.
    AdjustVolume {
        /// Sink or source.
        kind: Kind,
        /// The device's server index.
        index: u32,
        /// The step.
        delta: i32,
    },
    /// Flip one device's mute: its card's mute chip.
    ToggleMute {
        /// Sink or source.
        kind: Kind,
        /// The device's server index.
        index: u32,
    },
    /// Set an app stream's volume (the mixer's slider), clamped 0..=200.
    SetStreamVolume {
        /// The stream's server index.
        index: u32,
        /// The new volume.
        percent: u32,
    },
    /// Step an app stream's volume by `delta` % (scrolling on its card).
    AdjustStreamVolume {
        /// The stream's server index.
        index: u32,
        /// The step.
        delta: i32,
    },
    /// Flip an app stream's mute (its mute chip).
    ToggleStreamMute {
        /// The stream's server index.
        index: u32,
    },
    /// Make a device the default: a click on its card.
    SetDefault {
        /// Sink or source.
        kind: Kind,
        /// The device's stable name.
        name: String,
    },
}

/// The main thread's end: sends commands to the pulse thread.
#[derive(Debug, Default)]
pub(crate) struct Audio {
    /// Where commands go; `None` if the server couldn't be reached.
    commands: Option<mpsc::Sender<Command>>,
    /// Written to wake the pulse thread after queueing a command.
    wake: Option<UnixStream>,
}

impl Audio {
    /// Queues `command` and wakes the pulse thread. A no-op without a server.
    pub(crate) fn send(&mut self, command: Command) {
        let (Some(commands), Some(wake)) = (&self.commands, self.wake.as_mut()) else {
            return;
        };
        if commands.send(command).is_ok() {
            // Non-blocking: if the socket is full, a wake-up is pending anyway.
            let _pending = wake.write(&[1]);
        }
    }
}

/// Starts the pulse thread and waits (bounded) for it to connect. Snapshots
/// then arrive through the main loop and go to `App::audio_update`. If the
/// server can't be reached, says so once and returns a handle that does
/// nothing: the boxes show 0 %, as in C.
pub(crate) fn start(handle: &LoopHandle<'static, App>) -> Audio {
    match try_start(handle) {
        Ok(audio) => audio,
        Err(err) => {
            eprintln!("stickBar: {err} -- volume/mic controls disabled for this run");
            Audio::default()
        }
    }
}

/// [`start`], with the reason it failed.
fn try_start(handle: &LoopHandle<'static, App>) -> Result<Audio, String> {
    let (updates, updates_source) = channel::channel::<AudioState>();
    handle
        .insert_source(updates_source, |event, (), app| {
            if let channel::Event::Msg(state) = event {
                app.audio_update(state);
            }
        })
        .map(|_token| ())
        .map_err(|err| format!("couldn't watch the audio channel: {}", err.error))?;

    let (commands, command_queue) = mpsc::channel();
    let (wake, wake_reader) = UnixStream::pair().map_err(|err| format!("socket pair: {err}"))?;
    wake.set_nonblocking(true)
        .map_err(|err| format!("socket pair: {err}"))?;
    wake_reader
        .set_nonblocking(true)
        .map_err(|err| format!("socket pair: {err}"))?;
    let (ready, ready_wait) = mpsc::channel();
    // Named, so it shows in `top -H` and the tests' thread checks. Started
    // after the signal setup in `main`, so it inherits the SIGUSR1 block.
    std::thread::Builder::new()
        .name("pulse".to_owned())
        .spawn(move || pulse::run(&updates, &ready, &command_queue, &wake_reader))
        // Detached: it lives as long as the bar.
        .map(|_detached| ())
        .map_err(|err| format!("couldn't start the pulse thread: {err}"))?;

    match ready_wait.recv_timeout(CONNECT_TIMEOUT) {
        Ok(Ok(())) => Ok(Audio {
            commands: Some(commands),
            wake: Some(wake),
        }),
        Ok(Err(err)) => Err(err),
        Err(_) => Err("no answer from the PulseAudio-compatible server".to_owned()),
    }
}

/// libpulse's 100 % volume (`PA_VOLUME_NORM`).
const NORM: u64 = 0x1_0000;

/// A raw volume as a rounded percent of normal (C's `lround(avg * 100 / NORM)`),
/// in integers so no float casts are needed.
fn raw_to_percent(raw: u32) -> u32 {
    let percent = (u64::from(raw) * 100 + NORM / 2) / NORM;
    u32::try_from(percent).unwrap_or(u32::MAX)
}

/// A percent as a raw volume (C's `lround(percent / 100 * NORM)`).
fn percent_to_raw(percent: u32) -> u32 {
    let raw = (u64::from(percent) * NORM + 50) / 100;
    u32::try_from(raw).unwrap_or(u32::MAX)
}

/// `volume + delta`, clamped to 0..=`MAX_VOLUME`.
fn stepped(volume: u32, delta: i32) -> u32 {
    let next = (i64::from(volume) + i64::from(delta)).clamp(0, i64::from(MAX_VOLUME));
    u32::try_from(next).unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::{AudioState, Kind, Node, percent_to_raw, raw_to_percent, stepped};

    #[test]
    fn percent_round_trip() {
        assert_eq!(raw_to_percent(0x1_0000), 100);
        assert_eq!(raw_to_percent(percent_to_raw(75)), 75);
        assert_eq!(percent_to_raw(200), 0x2_0000);
        assert_eq!(raw_to_percent(32_767), 50); // 49.998 rounds up
    }

    #[test]
    fn steps_clamp() {
        assert_eq!(stepped(3, -5), 0);
        assert_eq!(stepped(198, 5), 200);
        assert_eq!(stepped(50, 5), 55);
    }

    #[test]
    fn shown_follows_the_default() {
        let node = |name: &str, volume| Node {
            name: name.to_owned(),
            volume,
            ..Node::default()
        };
        let mut state = AudioState {
            sinks: vec![node("a", 30), node("b", 70)],
            ..AudioState::default()
        };
        assert_eq!(state.shown(), [(0, false), (0, false)]); // no default yet
        state.default_sink = "b".to_owned();
        assert_eq!(state.default_node(Kind::Sink).map(|n| n.volume), Some(70));
        assert_eq!(state.shown(), [(70, false), (0, false)]);
    }
}
