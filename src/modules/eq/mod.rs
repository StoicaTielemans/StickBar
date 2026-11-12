//! The DAC EQ box (the C bar's `modules/eq/eq.c`): a small sparkline of the
//! EQ curve, then the active profile's name, in lavender (grey when the EQ is
//! off). It sits between network and volume, keeping the audio widgets
//! together. A right click opens its dropdown (`popup.rs`); a left click does
//! nothing, at the user's request, so a stray one can't pop it open.

pub(crate) mod popup;

use crate::{
    config::{BAR, COLORS, FONT},
    eq::{EqState, curve},
    render::{BoxRect, module_box::draw_rect, text::TextCache},
};

/// The sparkline's width: enough to read the shape (bass shelf vs treble
/// cuts) at a glance without crowding the neighbours.
const SPARK_W: f64 = 34.0;
/// Keeps the line clear of the box's 2 px border on a 20 px bar.
const SPARK_INSET_Y: f64 = 4.0;
/// Box edge to the sparkline.
const SPARK_INSET_X: f64 = 6.0;
/// Between the sparkline and the profile name.
const SPARK_GAP: f64 = 6.0;
/// The sparkline's dB span is at least ±6 dB (a few gentle treble cuts still
/// read as gentle), growing in 6 dB steps to fit heavier EQs.
const SPARK_MIN_RANGE_DB: f64 = 6.0;
/// See [`SPARK_MIN_RANGE_DB`].
const SPARK_RANGE_STEP_DB: f64 = 6.0;

/// The text next to the sparkline: the profile being applied (with `...`)
/// first, then the active one, then a fallback for an EQ that matches no
/// preset, or for no state file at all yet.
fn label(eq: &EqState) -> String {
    if let Some(pending) = &eq.pending {
        format!("{pending}...")
    } else if !eq.snap.available {
        "EQ --".to_owned()
    } else if eq.snap.profile.is_empty() {
        "custom".to_owned()
    } else {
        eq.snap.profile.clone()
    }
}

/// Draws the box ending at `right_x` (right region), spanning `top..top + height`.
pub(crate) fn render(
    cr: &cairo::Context,
    text: &mut TextCache,
    eq: &EqState,
    right_x: f64,
    (top, height): (f64, f64),
) -> Result<BoxRect, cairo::Error> {
    let label = label(eq);
    let text_w = text.width(&label, FONT.label, true);
    // The usual `label + padding` box, with the sparkline counted as label.
    let w = SPARK_W + SPARK_GAP + text_w + BAR.module_padding;
    let rect = BoxRect {
        x: right_x - w,
        y: top,
        w,
        h: height,
    };
    // A disabled EQ still has its curve stored on the DAC, so it's still
    // drawn, but greyed out so it's obvious it isn't being applied.
    let accent = if eq.snap.enabled {
        COLORS.lavender
    } else {
        COLORS.overlay0
    };
    draw_rect(cr, rect, COLORS.mantle, accent, BAR.module_border_width)?;

    let spark = BoxRect {
        x: rect.x + SPARK_INSET_X,
        y: top + SPARK_INSET_Y,
        w: SPARK_W,
        h: height - SPARK_INSET_Y * 2.0,
    };
    let range = curve::range_db(&eq.snap, SPARK_MIN_RANGE_DB, SPARK_RANGE_STEP_DB);
    curve::draw(
        cr,
        &eq.snap,
        spark,
        range,
        (accent, 1.5),
        Some(COLORS.surface1),
    )?;

    // The name, centred in the space right of the sparkline.
    let text_rect = BoxRect {
        x: spark.x + SPARK_W + SPARK_GAP,
        y: top,
        w: text_w,
        h: height,
    };
    text.draw_centered(cr, text_rect, &label, FONT.label, true, accent)?;
    Ok(rect)
}
