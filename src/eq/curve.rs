//! Drawing the EQ curve (the C bar's `eq/eq_curve.c`): pure cairo, no file or
//! process handling. Shared by the bar's 34 px sparkline and the dropdown's
//! full graph, which differ only in size, scale and the grid around it.

use super::Snapshot;
use crate::{config::Color, render::BoxRect};

/// The curve's span, matching the script's own (`CURVE_FMIN`/`CURVE_FMAX` in
/// `blackpearl-eq.py`): its points are log-spaced across exactly this.
const FREQ_MIN: f64 = 20.0;
/// See [`FREQ_MIN`].
const FREQ_MAX: f64 = 20_000.0;

/// The dB span shown above and below 0 dB: the smallest multiple of `step`
/// holding the curve's own peak and minimum, but never less than `min_range`.
/// A gentle EQ isn't stretched into a dramatic squiggle, and a heavy one
/// (+19 dB of bass) isn't clipped.
pub(crate) fn range_db(snap: &Snapshot, min_range: f64, step: f64) -> f64 {
    let extent = snap.peak_db.abs().max(snap.min_db.abs());
    ((extent / step).ceil() * step).max(min_range)
}

/// Where frequency `freq` (Hz) lands in `x..x + w`, log-scaled over
/// 20 Hz..20 kHz. The dropdown places its grid lines with it too, so they sit
/// on exactly the curve's scale.
pub(crate) fn freq_to_x(freq: f64, x: f64, w: f64) -> f64 {
    let freq = freq.clamp(FREQ_MIN, FREQ_MAX);
    x + w * (freq / FREQ_MIN).ln() / (FREQ_MAX / FREQ_MIN).ln()
}

/// Strokes the curve into `rect`, with 0 dB at its vertical middle and
/// ±`range_db` at its top and bottom edges. With `zero_line`, a 1 px 0 dB
/// line in that colour goes underneath first. Draws no curve if there isn't one.
pub(crate) fn draw(
    cr: &cairo::Context,
    snap: &Snapshot,
    rect: BoxRect,
    range_db: f64,
    (color, line_width): (Color, f64),
    zero_line: Option<Color>,
) -> Result<(), cairo::Error> {
    let mid_y = rect.y + rect.h / 2.0;
    let px_per_db = (rect.h / 2.0) / range_db;

    cr.save()?;
    // Clipped to the rect, so a point past ±range_db (only possible if a
    // caller passes a range smaller than the curve's extent) can't draw over
    // the box border around it.
    cr.rectangle(rect.x, rect.y, rect.w, rect.h);
    cr.clip();

    if let Some(zero_color) = zero_line {
        zero_color.set_source(cr);
        cr.set_line_width(1.0);
        // +0.5 so the 1 px line lands on one pixel row instead of straddling
        // two (cairo's coordinates are pixel edges, not pixel centres).
        cr.move_to(rect.x, mid_y.floor() + 0.5);
        cr.line_to(rect.x + rect.w, mid_y.floor() + 0.5);
        cr.stroke()?;
    }

    if snap.curve.len() >= 2 {
        color.set_source(cr);
        cr.set_line_width(line_width);
        cr.set_line_join(cairo::LineJoin::Round);
        cr.set_line_cap(cairo::LineCap::Round);
        for (i, &(freq, db)) in snap.curve.iter().enumerate() {
            let px = freq_to_x(freq, rect.x, rect.w);
            let py = mid_y - db * px_per_db; // +dB is up
            if i == 0 {
                cr.move_to(px, py);
            } else {
                cr.line_to(px, py);
            }
        }
        cr.stroke()?;
    }
    cr.restore()
}

#[cfg(test)]
mod tests {
    use super::{Snapshot, freq_to_x, range_db};

    #[test]
    fn range_grows_in_steps_with_a_floor() {
        let mut snap = Snapshot::default();
        assert!((range_db(&snap, 12.0, 6.0) - 12.0).abs() < 1e-9);
        snap.peak_db = 19.0;
        assert!((range_db(&snap, 12.0, 6.0) - 24.0).abs() < 1e-9);
        snap.peak_db = 1.0;
        snap.min_db = -7.5;
        assert!((range_db(&snap, 6.0, 6.0) - 12.0).abs() < 1e-9);
    }

    #[test]
    fn log_frequency_axis() {
        assert!((freq_to_x(20.0, 10.0, 300.0) - 10.0).abs() < 1e-9);
        assert!((freq_to_x(20_000.0, 10.0, 300.0) - 310.0).abs() < 1e-9);
        // 632 Hz is the geometric middle of 20 Hz..20 kHz.
        assert!((freq_to_x(632.455_532, 0.0, 300.0) - 150.0).abs() < 1e-3);
        assert!((freq_to_x(5.0, 0.0, 300.0)).abs() < 1e-9); // clamped
    }
}
