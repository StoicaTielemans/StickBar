//! The media dropdown's spectrum graphs: their audio, levels and animation
//! (the C bar's `visualizer/visualizer.c`; `docs/modules/media.md` has the
//! whole story). Up to [`MAX_SOURCES`] "slots", one per player card, each
//! with smoothed left/right band levels and peak markers, advanced by a
//! 15 fps timer that only runs while a slot is capturing or still settling.
//! With the dropdown closed nothing runs at all.
//!
//! **Synthetic mode** (`STICKBAR_TEST_VISUALIZER_SYNTH`, set by
//! `tests/test_visualizer.py`): no `PipeWire` at all; an active slot "hears"
//! sine tones set over the test-control channel (`vis-tone`). Everything
//! after the audio (slots turning on and off, the FFT, smoothing, drawing)
//! is the real path, so the test never touches the user's audio.

pub(crate) mod spectrum;

use spectrum::{BANDS, FFT_SIZE, Spectrum};

/// Graph slots: cards past this many get no graph.
pub(crate) const MAX_SOURCES: usize = 4;
/// Frames per second (the user's pick; 30 cost twice the CPU).
pub(crate) const FPS: u32 = 15;
/// The capture rate every stream is converted to.
const CAPTURE_RATE: f32 = 48_000.0;
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
        }
    }
}

/// The graphs' audio side, on the main thread.
pub(crate) struct Visualizer {
    /// Graphs are shown at all (else the dropdown has no graph rows).
    available: bool,
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
            // Real capture arrives with the PipeWire streams; until then
            // only the synthetic mode has anything to show.
            available: subsurfaces && !disabled && synth,
            synth,
            only: env_set("STICKBAR_TEST_VISUALIZER_ONLY"),
            slots: Default::default(),
            spectrum: None,
            window: [vec![0.0; FFT_SIZE], vec![0.0; FFT_SIZE]],
        }
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

    /// Turns `slot`'s capture on or off. Returns whether the animation timer
    /// must run (a slot just turned on); turning off lets the levels fall,
    /// and [`Self::pump`] says when everything has settled.
    pub(crate) fn set_active(&mut self, slot: usize, active: bool) -> bool {
        if !self.available {
            return false;
        }
        let Some(s) = self.slots.get_mut(slot) else {
            return false;
        };
        if s.active == active {
            return false;
        }
        s.active = active;
        if active {
            s.settled = false;
        }
        active
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
                    *s = Slot {
                        tones: s.tones,
                        ..Slot::default()
                    };
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

    /// One frame of analysis for slot `i`: both channels' windows, the FFT,
    /// and the smoothing. Returns whether anything moved.
    fn analyse(&mut self, i: usize) -> bool {
        let Some(slot) = self.slots.get(i) else {
            return false;
        };
        let (active, tones) = (slot.active, slot.tones);
        // Synthetic audio while active; silence once off, as a paused real
        // stream gives.
        for (window, hz) in self.window.iter_mut().zip(tones) {
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
