//! The media visualizer's capture streams (the `PipeWire` half of the C bar's
//! `visualizer/visualizer.c`). They live on the `pipewire` thread, beside the
//! privacy graph, on the same connection: the C bar opened a second
//! connection and thread for them; here neither is needed.
//!
//! One stream per active slot, capturing its player's own playback stream
//! (or, if it can't be found, the default output's mix), stereo float at
//! 48 kHz. **Off means gone, not paused**: a paused stream stays a node in the
//! graph, linked to its target, for as long as the bar runs.

use std::sync::{Arc, Mutex};

use pipewire::{
    self as pw,
    core::CoreRc,
    spa::{
        self,
        param::audio::{AudioFormat, AudioInfoRaw},
        pod::{Object, Pod, Value, serialize::PodSerializer},
        utils::{Direction, SpaTypes},
    },
    stream::{StreamFlags, StreamListener, StreamRc, StreamState},
};

use super::graph::VISUALIZER_NODE_NAME;
use crate::visualizer::spectrum::FFT_SIZE;

/// The capture rate: `PipeWire`'s adapter converts whatever the source runs at.
const CAPTURE_RATE: u32 = 48_000;
/// `target` for a slot capturing the default output's mix (the sink monitor).
pub(crate) const TARGET_MIX: u32 = 0;
/// Frames in the newest-samples rings: one analysis window.
const RING_LEN: usize = FFT_SIZE;

/// The newest [`FFT_SIZE`] samples per channel of one slot's capture.
///
/// **Why `Arc<Mutex<..>>` ([`SharedRings`]):** the capture stream's `process`
/// callback writes them on the `pipewire` thread, and the visualizer reads a
/// window out of them on the main thread 15 times a second. Two threads, one
/// buffer: a mutex held for a buffer's worth of copying on each side.
#[derive(Debug)]
pub(crate) struct Rings {
    /// Left and right, each a ring with `pos` the next write (the oldest).
    pub(crate) channels: [Vec<f32>; 2],
    /// The next write position.
    pub(crate) pos: usize,
    /// Frames written since the visualizer last looked.
    pub(crate) fresh: u32,
}

impl Default for Rings {
    fn default() -> Self {
        Self {
            channels: [vec![0.0; RING_LEN], vec![0.0; RING_LEN]],
            pos: 0,
            fresh: 0,
        }
    }
}

impl Rings {
    /// Silence: the old window is no longer the slot's audio.
    pub(crate) fn clear(&mut self) {
        for channel in &mut self.channels {
            channel.fill(0.0);
        }
        self.fresh = 0;
    }

    /// Copies the window, oldest sample first, into `out` (one per channel).
    pub(crate) fn window(&self, out: &mut [Vec<f32>; 2]) {
        for (dst, ring) in out.iter_mut().zip(&self.channels) {
            let (newer, older) = ring.split_at(self.pos.min(ring.len()));
            for (d, s) in dst.iter_mut().zip(older.iter().chain(newer)) {
                *d = *s;
            }
        }
    }

    /// Appends interleaved F32LE stereo frames.
    fn push(&mut self, bytes: &[u8]) {
        let mut frames = 0_u32;
        for frame in bytes.as_chunks::<8>().0 {
            let [l0, l1, l2, l3, r0, r1, r2, r3] = *frame;
            let samples = [
                f32::from_le_bytes([l0, l1, l2, l3]),
                f32::from_le_bytes([r0, r1, r2, r3]),
            ];
            for (channel, sample) in self.channels.iter_mut().zip(samples) {
                if let Some(slot) = channel.get_mut(self.pos) {
                    *slot = sample;
                }
            }
            self.pos = (self.pos + 1) % RING_LEN;
            frames = frames.saturating_add(1);
        }
        self.fresh = self.fresh.saturating_add(frames);
    }
}

/// A slot's rings, shared between the threads (see [`Rings`]).
pub(crate) type SharedRings = Arc<Mutex<Rings>>;

/// What the visualizer asks of the `pipewire` thread.
#[derive(Debug)]
pub(crate) enum CaptureCommand {
    /// Slot `slot` should capture process `pid`'s audio into `rings` (0 =
    /// the mix), or nothing (`None`).
    Slot {
        /// Which slot.
        slot: usize,
        /// (player pid, its rings), or off.
        want: Option<(u32, SharedRings)>,
    },
}

/// A live capture stream and its listener (dropping both destroys it).
pub(crate) type Capture = (StreamRc, StreamListener<()>);

/// Creates and connects a capture stream for `target` (a playback stream's
/// `object.serial`, or [`TARGET_MIX`]) writing into `rings`.
pub(crate) fn create_stream(
    core: &CoreRc,
    target: u32,
    rings: SharedRings,
) -> Result<Capture, pw::Error> {
    let mut props = pw::properties::properties! {
        "media.type" => "Audio",
        "media.category" => "Capture",
        "node.name" => VISUALIZER_NODE_NAME,
        "node.description" => "stickBar media visualizer",
        // Keeps nothing awake on the bar's account.
        "node.passive" => "true",
        // ~21 ms per buffer: plenty for a 15 fps graph.
        "node.latency" => "1024/48000",
    };
    if target == TARGET_MIX {
        // The sink's monitor, not a source: with no target the session
        // manager links it to the default sink and follows it when that
        // changes.
        props.insert("stream.capture.sink", "true");
        // Tests may point the mix at a throwaway null sink.
        if let Some(test_target) = std::env::var("STICKBAR_TEST_VISUALIZER_TARGET")
            .ok()
            .filter(|v| !v.is_empty())
        {
            props.insert("target.object", test_target);
        }
    } else {
        // One app's stream, and never anything else: without dont-fallback a
        // target that's gone (the app closed it between tracks) makes the
        // session manager link the capture to the default *source*, i.e. the
        // microphone. A gone target leaves it unlinked (silent) instead,
        // until the slot is re-resolved.
        props.insert("target.object", target.to_string());
        props.insert("node.dont-fallback", "true");
        props.insert("node.dont-reconnect", "true");
    }
    let stream = StreamRc::new(core.clone(), VISUALIZER_NODE_NAME, props)?;
    let listener = stream
        .add_local_listener_with_user_data(())
        .state_changed(|_stream, (), _old, new| {
            if let StreamState::Error(err) = new {
                eprintln!("stickBar: visualizer capture stream error: {err}");
            }
        })
        .process(move |stream, ()| {
            let Some(mut buffer) = stream.dequeue_buffer() else {
                return;
            };
            let Some(data) = buffer.datas_mut().first_mut() else {
                return;
            };
            let (offset, size) = (data.chunk().offset(), data.chunk().size());
            let Some(bytes) = data.data() else {
                return;
            };
            let start = usize::try_from(offset)
                .unwrap_or(usize::MAX)
                .min(bytes.len());
            let end = start
                .saturating_add(usize::try_from(size).unwrap_or(0))
                .min(bytes.len());
            if let (Some(samples), Ok(mut rings)) = (bytes.get(start..end), rings.lock()) {
                rings.push(samples);
            }
        })
        .register()?;

    let mut info = AudioInfoRaw::new();
    info.set_format(AudioFormat::F32LE);
    info.set_rate(CAPTURE_RATE);
    info.set_channels(2);
    // SPA_AUDIO_MAX_CHANNELS (64): the type of `set_position`'s argument
    // makes the compiler check it.
    let mut position = [0_u32; 64];
    let [fl, fr, ..] = &mut position;
    *fl = spa::sys::SPA_AUDIO_CHANNEL_FL;
    *fr = spa::sys::SPA_AUDIO_CHANNEL_FR;
    info.set_position(position);
    let object = Object {
        type_: SpaTypes::ObjectParamFormat.as_raw(),
        id: spa::param::ParamType::EnumFormat.as_raw(),
        properties: info.into(),
    };
    let bytes = PodSerializer::serialize(std::io::Cursor::new(Vec::new()), &Value::Object(object))
        .map_err(|_err| pw::Error::CreationFailed)?
        .0
        .into_inner();
    let pod = Pod::from_bytes(&bytes).ok_or(pw::Error::CreationFailed)?;
    stream.connect(
        Direction::Input,
        None,
        StreamFlags::AUTOCONNECT | StreamFlags::MAP_BUFFERS,
        &mut [pod],
    )?;
    Ok((stream, listener))
}

/// Whether process `pid` is `ancestor` or a descendant of it, walking
/// `PPid:` in `/proc/<pid>/status` (bounded: real trees are shallow, and it
/// guards against a loop). Browsers play audio from a child process, while
/// their MPRIS name belongs to the main one.
pub(crate) fn descends(pid: u32, ancestor: u32) -> bool {
    let mut current = pid;
    for _ in 0..16 {
        if current == ancestor {
            return true;
        }
        if current <= 1 {
            return false;
        }
        let Some(parent) = parent_of(current) else {
            return false;
        };
        current = parent;
    }
    false
}

/// `pid`'s parent, from `/proc/<pid>/status`.
fn parent_of(pid: u32) -> Option<u32> {
    let status = std::fs::read_to_string(format!("/proc/{pid}/status")).ok()?;
    status
        .lines()
        .find_map(|line| line.strip_prefix("PPid:"))
        .and_then(|v| v.trim().parse().ok())
}

#[cfg(test)]
mod tests {
    use super::{Rings, descends};

    #[test]
    fn rings_keep_the_newest_window_oldest_first() {
        let mut rings = Rings::default();
        // Two frames: L 1.0 / R 2.0, then L 3.0 / R 4.0.
        let mut bytes = Vec::new();
        for v in [1.0_f32, 2.0, 3.0, 4.0] {
            bytes.extend_from_slice(&v.to_le_bytes());
        }
        rings.push(&bytes);
        assert_eq!(rings.fresh, 2);
        let mut window = [vec![0.0; 4096], vec![0.0; 4096]];
        rings.window(&mut window);
        assert_eq!(&window[0][4094..], &[1.0, 3.0]);
        assert_eq!(&window[1][4094..], &[2.0, 4.0]);
    }

    #[test]
    fn process_ancestry() {
        let me = std::process::id();
        assert!(descends(me, me));
        let parent = super::parent_of(me).unwrap();
        assert!(descends(me, parent));
        assert!(!descends(parent, me));
        assert!(!descends(me, u32::MAX));
    }
}
