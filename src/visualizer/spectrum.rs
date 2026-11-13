//! Samples to frequency-band levels for the media dropdown's spectrum graphs
//! (the C bar's `visualizer/spectrum.c`). Pure math, no `PipeWire` or drawing
//! types, so it's unit-tested on its own.
//!
//! A Hann-windowed 4096-sample FFT (`realfft`: rustfft's real-input wrapper,
//! the "half-size complex FFT + untangle" trick C hand-wrote), then 82 bands
//! spaced evenly on a log scale from 50 Hz to 12 kHz, the way hearing (and
//! cava) spaces them. Each band's level is the loudest bin in it, in dBFS
//! with a +3 dB/octave tilt, mapped onto 0..1.

use std::sync::Arc;

use realfft::{RealFftPlanner, RealToComplex, num_complex::Complex};

/// Samples per window: ~85 ms at 48 kHz, ~11.7 Hz per bin. Fine enough that
/// the lowest of the 82 bands mostly get bins of their own (2048 left the
/// bottom dozen sharing).
pub(crate) const FFT_SIZE: usize = 4096;
/// Bands: one per dot column of the dropdown's graph (41 cells x 2 dots).
pub(crate) const BANDS: usize = 82;
/// The lowest band's lower edge: below it is mostly rumble.
const BAND_LO_HZ: f32 = 50.0;
/// The highest band's upper edge: above it most music has little energy.
const BAND_HI_HZ: f32 = 12_000.0;
/// dBFS shown as an empty column (after the tilt), picked by ear.
const FLOOR_DB: f32 = -62.0;
/// dBFS shown as a full column.
const CEIL_DB: f32 = -14.0;
/// Music falls off about 3 dB per octave going up; without this tilt the
/// bass columns would be tall and the treble ones nearly dead.
const TILT_DB_PER_OCTAVE: f32 = 3.0;
/// The tilt's zero point.
const TILT_PIVOT_HZ: f32 = 500.0;

/// One band: its FFT bins (inclusive) and its tilt.
#[derive(Clone, Copy, Debug, PartialEq)]
struct Band {
    /// First bin.
    lo: usize,
    /// Last bin.
    hi: usize,
    /// Added to its level in dB.
    tilt_db: f32,
}

/// The analyser: the FFT plan, its buffers, and the band layout, all made
/// once (for a fixed size and rate).
pub(crate) struct Spectrum {
    /// The forward FFT.
    fft: Arc<dyn RealToComplex<f32>>,
    /// The Hann window.
    window: Vec<f32>,
    /// The windowed input (the FFT also uses it as scratch).
    input: Vec<f32>,
    /// The FFT's output.
    output: Vec<Complex<f32>>,
    /// The bands, for [`BANDS`] at the capture rate.
    bands: Vec<Band>,
}

impl Spectrum {
    /// Plans the FFT and lays out the bands for samples at `rate` Hz.
    pub(crate) fn new(rate: f32) -> Self {
        let fft = RealFftPlanner::<f32>::new().plan_fft_forward(FFT_SIZE);
        let n = count_f32(FFT_SIZE);
        let window = (0..FFT_SIZE)
            .map(|i| 0.5 - 0.5 * (std::f32::consts::TAU * count_f32(i) / (n - 1.0)).cos())
            .collect();
        Self {
            input: fft.make_input_vec(),
            output: fft.make_output_vec(),
            fft,
            window,
            bands: layout_bands(rate),
        }
    }

    /// One window of samples (oldest first, -1..1) to raw (unsmoothed) band
    /// levels, 0..1. Smoothing over time is the caller's: it depends on the
    /// frame rate, which this doesn't know.
    pub(crate) fn levels(&mut self, samples: &[f32], out: &mut [f32; BANDS]) {
        for ((dst, &s), &w) in self.input.iter_mut().zip(samples).zip(&self.window) {
            *dst = s * w;
        }
        if let Err(err) = self.fft.process(&mut self.input, &mut self.output) {
            eprintln!("stickBar: visualizer FFT failed: {err}");
            out.fill(0.0);
            return;
        }
        // |X[k]| times this is the amplitude (0..1) of a sine on bin k: a
        // one-sided spectrum doubles it, and the Hann window's coherent gain
        // of 0.5 halves the N it would be scaled by. So a full-scale sine
        // reads 0 dBFS, and FLOOR_DB/CEIL_DB are plain dBFS.
        let amp_scale = 4.0 / count_f32(FFT_SIZE);
        for (level, band) in out.iter_mut().zip(&self.bands) {
            // The peak, not the average: a tone in a wide high band would
            // otherwise look much quieter than in a narrow low one.
            let peak = self
                .output
                .get(band.lo..=band.hi)
                .unwrap_or_default()
                .iter()
                .map(|c| c.norm() * amp_scale)
                .fold(0.0_f32, f32::max);
            let db = 20.0 * (peak + 1e-9).log10() + band.tilt_db;
            *level = ((db - FLOOR_DB) / (CEIL_DB - FLOOR_DB)).clamp(0.0, 1.0);
        }
    }
}

/// A count as f32 (exact: every count here is far below 2^24).
fn count_f32(n: usize) -> f32 {
    u16::try_from(n).map_or(f32::MAX, f32::from)
}

/// `value` rounded down / up to a bin index (non-negative, small).
fn bin(value: f32, up: bool) -> usize {
    let rounded = if up { value.ceil() } else { value.floor() };
    usize::try_from(crate::render::whole_px(f64::from(rounded))).unwrap_or(0)
}

/// The [`BANDS`] bands' bins and tilts for samples at `rate` Hz.
fn layout_bands(rate: f32) -> Vec<Band> {
    let bin_hz = rate / count_f32(FFT_SIZE);
    let max_bin = FFT_SIZE / 2 - 1;
    let count = count_f32(BANDS);
    (0..BANDS)
        .map(|b| {
            let edge = |i: usize| BAND_LO_HZ * (BAND_HI_HZ / BAND_LO_HZ).powf(count_f32(i) / count);
            let (lo_hz, hi_hz) = (edge(b), edge(b + 1));
            // Bin 0 is DC, not sound.
            let lo = bin(lo_hz / bin_hz, false).max(1);
            let hi = bin(hi_hz / bin_hz, true).min(max_bin).max(lo);
            let center_hz = (lo_hz * hi_hz).sqrt();
            Band {
                lo,
                hi,
                tilt_db: TILT_DB_PER_OCTAVE * (center_hz / TILT_PIVOT_HZ).log2(),
            }
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::{BANDS, FFT_SIZE, Spectrum, bin, count_f32, layout_bands};

    /// The band a tone falls in, as `tests/test_visualizer.py` works it out.
    fn band_of(hz: f32) -> usize {
        bin(
            count_f32(BANDS) * (hz / 50.0).ln() / (12_000.0_f32 / 50.0).ln(),
            false,
        )
    }

    /// A sine of amplitude 0.5.
    fn sine(hz: f32) -> Vec<f32> {
        (0..FFT_SIZE)
            .map(|i| 0.5 * (std::f32::consts::TAU * hz * count_f32(i) / 48_000.0).sin())
            .collect()
    }

    #[test]
    fn a_tone_peaks_in_its_band() {
        let mut spectrum = Spectrum::new(48_000.0);
        let mut out = [0.0; BANDS];
        spectrum.levels(&sine(1000.0), &mut out);
        let b = band_of(1000.0);
        assert!(out[b] > 0.9, "level {}", out[b]);
        let rest = out
            .iter()
            .enumerate()
            .filter(|(i, _)| i.abs_diff(b) > 2)
            .map(|(_, v)| *v)
            .fold(0.0, f32::max);
        assert!(out[b] > rest + 0.3);
        spectrum.levels(&vec![0.0; FFT_SIZE], &mut out);
        assert!(out.iter().all(|&v| v == 0.0));
    }

    #[test]
    fn bands_cover_50hz_to_12khz() {
        let bands = layout_bands(48_000.0);
        assert_eq!(bands.len(), BANDS);
        // 11.72 Hz bins: 50 Hz is bin 4, 12 kHz bin 1024.
        assert_eq!(bands[0].lo, 4);
        assert_eq!(bands[BANDS - 1].hi, 1024);
        assert!(bands.iter().all(|b| b.hi >= b.lo && b.lo >= 1));
    }
}
