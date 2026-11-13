//! The media dropdown's spectrum graphs: their audio, levels and animation
//! (the C bar's `visualizer/visualizer.c`; `docs/modules/media.md` has the
//! whole story). Up to [`MAX_SOURCES`] "slots", one per player card, each
//! with smoothed left/right band levels and peak markers, advanced by a
//! 15 fps timer that only runs while a slot is capturing or still settling.
//! With the dropdown closed nothing runs at all.
//!
//! **Real audio** comes from the `pipewire` thread (`src/pipewire/capture.rs`):
//! a slot turned on sends it the card's player pid, and its capture stream
//! fills that slot's shared rings, which each frame reads a window from.
//!
//! **Synthetic mode** (`STICKBAR_TEST_VISUALIZER_SYNTH`, set by
//! `tests/test_visualizer.py`): no `PipeWire` at all; an active slot "hears"
//! sine tones set over the test-control channel (`vis-tone`). Everything
//! after the audio (slots turning on and off, the FFT, smoothing, drawing)
//! is the real path, so the test never touches the user's audio.

pub(crate) mod spectrum;

use spectrum::{BANDS, FFT_SIZE, Spectrum};

use crate::pipewire::{
    Capture,
    capture::{CaptureCommand, SharedRings},
};

/// Graph slots: cards past this many get no graph.
pub(crate) const MAX_SOURCES: usize = 4;
/// Frames per second (the user's pick; 30 cost twice the CPU).
pub(crate) const FPS: u32 = 15;
/// The capture rate every stream is converted to.
const CAPTURE_RATE: f32 = 48_000.0;
/// Frames without new samples after which a slot's rings count as silence.
/// "Playing" doesn't mean audio flows: between tracks or on a stalled stream
/// `PipeWire` just stops delivering, and the last window would stay frozen,
/// the graph with it. 2 frames (~133 ms) is several buffer periods, so
/// ordinary jitter never trips it.
const STALE_FRAMES: u8 = 2;
/// Per-frame rise toward the newest level: most of the way at once, so beats
/// land on the beat (0.6 per 30 fps frame, applied twice).
const ATTACK: f32 = 0.84;
/// Fall per second: a full column empties in ~0.7 s at any frame rate.
const FALL_PER_SECOND: f32 = 1.5;
/// A new peak is held this long...
const PEAK_HOLD_SECONDS: f32 = 0.5;
/// ...then falls this fast: slower than the levels, so peaks trail them.
const PEAK_FALL_PER_SECOND: f32 = 0.6;

/// A level or peak per band, per channel (left, right).
pub(crate) type Levels = [[f32; BANDS]; 2];

/// One graph slot.
#[derive(Debug)]
struct Slot {
    /// Capturing (or, in synth mode, playing its tones).
    active: bool,
    /// Smoothed levels, 0..1.
    levels: Levels,
    /// Peak markers, >= the levels.
    peaks: Levels,
    /// Frames before each peak starts falling.
    hold: [[u8; BANDS]; 2],
    /// Every level and peak is 0.
    settled: bool,
    /// Synth mode: each channel's tone in Hz (0 = silence).
    tones: [f32; 2],
    /// The player pid it was last pointed at (0 = unknown: the mix).
    pid: u32,
    /// Frames in a row without new samples.
    stale: u8,
    /// What its capture follows (a playback stream's serial, 0 = the mix).
    target: u32,
    /// Where its capture writes.
    rings: SharedRings,
}

impl Default for Slot {
    fn default() -> Self {
        Self {
            active: false,
            levels: [[0.0; BANDS]; 2],
            peaks: [[0.0; BANDS]; 2],
            hold: [[0; BANDS]; 2],
            settled: true,
            tones: [0.0; 2],
            pid: 0,
            stale: 0,
            target: 0,
            rings: SharedRings::default(),
        }
    }
}

/// The graphs' audio side, on the main thread.
pub(crate) struct Visualizer {
    /// Graphs are shown at all (else the dropdown has no graph rows).
    available: bool,
    /// The compositor has subsurfaces and the tests didn't turn it off.
    allowed: bool,
    /// Sends slot commands to the `pipewire` thread (real capture).
    capture: Option<Capture>,
    /// Synthetic tones instead of captured audio (tests).
    synth: bool,
    /// Tests: only players whose app name starts with this are captured.
    only: Option<String>,
    /// The slots.
    slots: [Slot; MAX_SOURCES],
    /// The FFT and bands, shared by every slot (made on first use).
    spectrum: Option<Spectrum>,
    /// One window per channel, reused.
    window: [Vec<f32>; 2],
}

impl std::fmt::Debug for Visualizer {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Visualizer")
            .field("available", &self.available)
            .field("synth", &self.synth)
            .finish_non_exhaustive()
    }
}

impl Default for Visualizer {
    /// Unavailable until `App::new` knows whether subsurfaces exist.
    fn default() -> Self {
        Self::new(false)
    }
}

/// An environment variable that's set to something non-empty.
fn env_set(name: &str) -> Option<String> {
    std::env::var(name).ok().filter(|v| !v.is_empty())
}

impl Visualizer {
    /// Reads the test switches. `subsurfaces`: the compositor offers
    /// `wl_subcompositor`, which the graphs are drawn on.
    pub(crate) fn new(subsurfaces: bool) -> Self {
        let disabled = env_set("STICKBAR_TEST_NO_VISUALIZER").is_some();
        let synth = env_set("STICKBAR_TEST_VISUALIZER_SYNTH").is_some();
        Self {
            // Real capture needs the `pipewire` thread: see `set_capture`.
            available: subsurfaces && !disabled && synth,
            allowed: subsurfaces && !disabled,
            capture: None,
            synth,
            only: env_set("STICKBAR_TEST_VISUALIZER_ONLY"),
            slots: Default::default(),
            spectrum: None,
            window: [vec![0.0; FFT_SIZE], vec![0.0; FFT_SIZE]],
        }
    }

    /// Hands over the `pipewire` thread's command handle: real capture is
    /// possible from now on (`None`: it couldn't start).
    pub(crate) fn set_capture(&mut self, capture: Option<Capture>) {
        self.capture = capture;
        self.available = self.allowed && (self.synth || self.capture.is_some());
    }

    /// The `pipewire` thread couldn't connect: no graphs from now on.
    pub(crate) fn set_unavailable(&mut self) {
        if !self.synth {
            self.capture = None;
            self.available = false;
        }
    }

    /// Slot `slot` now captures `target` (the `pipewire` thread says).
    pub(crate) fn set_target(&mut self, slot: usize, target: u32) {
        if let Some(s) = self.slots.get_mut(slot) {
            s.target = target;
        }
    }

    /// What slot `slot` captures (the tests' `dump visualizer`).
    pub(crate) fn target(&self, slot: usize) -> u32 {
        self.slots.get(slot).map_or(0, |s| s.target)
    }

    /// Whether the dropdown shows graphs.
    pub(crate) const fn available(&self) -> bool {
        self.available
    }

    /// Whether `app`'s audio may be captured (tests limit it to their fakes).
    pub(crate) fn capture_allowed(&self, app: &str) -> bool {
        self.only
            .as_deref()
            .is_none_or(|prefix| app.starts_with(prefix))
    }

    /// Slot `slot`'s smoothed levels.
    pub(crate) fn levels(&self, slot: usize) -> Option<&Levels> {
        self.slots.get(slot).map(|s| &s.levels)
    }

    /// Slot `slot`'s peak markers.
    pub(crate) fn peaks(&self, slot: usize) -> Option<&Levels> {
        self.slots.get(slot).map(|s| &s.peaks)
    }

    /// Synth mode: `slot`'s `channel` plays a sine at `hz` (0 = silence)
    /// while active (the tests' `vis-tone`).
    pub(crate) fn set_tone(&mut self, slot: usize, channel: usize, hz: f32) {
        if let Some(tone) = self
            .slots
            .get_mut(slot)
            .and_then(|s| s.tones.get_mut(channel))
        {
            *tone = hz;
        }
    }

    /// Turns `slot`'s capture on or off, pointed at player process `pid`.
    /// Cheap and idempotent (called every loop pass): only a change does
    /// anything. Returns whether the animation timer must run (a slot just
    /// turned on); turning off lets the levels fall, and [`Self::pump`] says
    /// when everything has settled.
    pub(crate) fn set_source(&mut self, slot: usize, active: bool, pid: u32) -> bool {
        if !self.available {
            return false;
        }
        let synth = self.synth;
        let Some(s) = self.slots.get_mut(slot) else {
            return false;
        };
        let retarget = !synth && active && s.active && s.pid != pid;
        if s.active == active && !retarget {
            return false;
        }
        if !synth && let Some(capture) = self.capture.as_ref() {
            capture.send(CaptureCommand::Slot {
                slot,
                want: active.then(|| (pid, std::sync::Arc::clone(&s.rings))),
            });
        }
        let started = active && !s.active;
        s.active = active;
        s.pid = pid;
        s.stale = 0;
        if active {
            s.settled = false;
        }
        started
    }

    /// One animation frame. Slots in `ready` (their graph can show a frame
    /// now) are analysed and smoothed; capturing slots that aren't ready skip
    /// this tick; slots that are off and not in `shown` (no graph on screen)
    /// drop straight to rest. Returns the slots whose levels changed, and
    /// whether the timer must keep running (something capturing or settling).
    pub(crate) fn pump(&mut self, ready: u32, shown: u32) -> (u32, bool) {
        let mut changed = 0;
        let mut busy = false;
        for i in 0..MAX_SOURCES {
            let bit = 1_u32 << i;
            let Some(slot) = self.slots.get(i) else {
                continue;
            };
            if !slot.active && slot.settled {
                continue;
            }
            if !slot.active && shown & bit == 0 {
                // Off and off screen: nothing will ask for its frames.
                if let Some(s) = self.slots.get_mut(i) {
                    s.levels = [[0.0; BANDS]; 2];
                    s.peaks = [[0.0; BANDS]; 2];
                    s.hold = [[0; BANDS]; 2];
                    s.settled = true;
                }
                continue;
            }
            if ready & bit != 0 && self.analyse(i) {
                changed |= bit;
            }
            if self.slots.get(i).is_some_and(|s| s.active || !s.settled) {
                busy = true;
            }
        }
        (changed, busy)
    }

    /// Copies slot `i`'s newest window out of its rings (held only for the
    /// copy, so the capture is never held up), counting it as silence once
    /// no new samples have come for [`STALE_FRAMES`] frames.
    fn read_rings(&mut self, i: usize) {
        let Some(slot) = self.slots.get_mut(i) else {
            return;
        };
        let Ok(mut rings) = slot.rings.lock() else {
            return;
        };
        slot.stale = if rings.fresh > 0 {
            0
        } else {
            slot.stale.saturating_add(1)
        };
        rings.fresh = 0;
        if slot.stale >= STALE_FRAMES {
            rings.clear();
        }
        rings.window(&mut self.window);
    }

    /// One frame of analysis for slot `i`: both channels' windows, the FFT,
    /// and the smoothing. Returns whether anything moved.
    fn analyse(&mut self, i: usize) -> bool {
        let Some(slot) = self.slots.get(i) else {
            return false;
        };
        let (active, tones) = (slot.active, slot.tones);
        if !self.synth {
            self.read_rings(i);
        }
        // Synthetic audio while active; silence once off, as a paused real
        // stream gives.
        for (window, hz) in self.window.iter_mut().zip(tones).filter(|_| self.synth) {
            let hz = if active { hz } else { 0.0 };
            // The window is 4096 samples: every index fits a u16.
            for (n, sample) in (0_u16..).zip(window.iter_mut()) {
                *sample = if hz > 0.0 {
                    0.5 * (std::f32::consts::TAU * hz * f32::from(n) / CAPTURE_RATE).sin()
                } else {
                    0.0
                };
            }
        }
        let spectrum = self
            .spectrum
            .get_or_insert_with(|| Spectrum::new(CAPTURE_RATE));
        let mut changed = false;
        let mut settled = true;
        let Some(target) = self.slots.get_mut(i) else {
            return false;
        };
        for (((window, levels), peaks), hold) in self
            .window
            .iter()
            .zip(&mut target.levels)
            .zip(&mut target.peaks)
            .zip(&mut target.hold)
        {
            let mut raw = [0.0; BANDS];
            spectrum.levels(window, &mut raw);
            let (c, s) = smooth((levels, peaks, hold), &raw);
            changed |= c;
            settled &= s;
        }
        target.settled = settled;
        changed
    }
}

/// Moves one channel's `levels` toward `raw` by one frame (fast rise, fixed
/// fall), and its peaks after them (held, then falling slower). Returns
/// (anything moved, everything is at rest).
fn smooth(
    (levels, peaks, hold): (&mut [f32; BANDS], &mut [f32; BANDS], &mut [u8; BANDS]),
    raw: &[f32; BANDS],
) -> (bool, bool) {
    let fps = f32::from(u16::try_from(FPS).unwrap_or(15));
    let fall = FALL_PER_SECOND / fps;
    let peak_fall = PEAK_FALL_PER_SECOND / fps;
    // 0.5 s at 15 fps: 7.5, rounded as C's `(int)(x + 0.5)`.
    let hold_frames = u8::try_from(crate::render::whole_px(f64::from(
        PEAK_HOLD_SECONDS * fps + 0.5,
    )))
    .unwrap_or(0);
    let mut changed = false;
    let mut settled = true;
    for (((level, peak), held), &target) in levels
        .iter_mut()
        .zip(peaks.iter_mut())
        .zip(hold.iter_mut())
        .zip(raw)
    {
        let cur = *level;
        let next = if target > cur {
            cur + (target - cur) * ATTACK
        } else {
            (cur - fall).max(target)
        };
        *level = next;
        let old_peak = *peak;
        if next >= *peak {
            *peak = next;
            *held = hold_frames;
        } else if *held > 0 {
            *held -= 1;
        } else {
            *peak = (*peak - peak_fall).max(next);
        }
        if (next - cur).abs() > 1e-4 || (*peak - old_peak).abs() > 1e-4 {
            changed = true;
        }
        if next > 0.0 || *peak > 0.0 {
            settled = false;
        }
    }
    (changed, settled)
}

#[cfg(test)]
mod tests {
    use super::{BANDS, smooth};

    #[test]
    fn rises_fast_falls_slowly_and_settles() {
        let (mut levels, mut peaks, mut hold) = ([0.0; BANDS], [0.0; BANDS], [0; BANDS]);
        let full = [1.0; BANDS];
        let (changed, settled) = smooth((&mut levels, &mut peaks, &mut hold), &full);
        assert!(changed && !settled);
        assert!((levels[0] - 0.84).abs() < 1e-6);
        // Silence: the level falls 0.1 per frame, the peak holds 8 frames.
        let silence = [0.0; BANDS];
        let _moved = smooth((&mut levels, &mut peaks, &mut hold), &silence);
        assert!((levels[0] - 0.74).abs() < 1e-5);
        assert!((peaks[0] - 0.84).abs() < 1e-6);
        let mut frames = 0;
        while !smooth((&mut levels, &mut peaks, &mut hold), &silence).1 {
            frames += 1;
            assert!(frames < 100);
        }
        assert!(levels.iter().chain(&peaks).all(|&v| v == 0.0));
    }
}
