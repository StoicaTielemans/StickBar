//! A segmented "how full" bar, btop's ▰▰▱▱ meters (the C bar's
//! `render/meter_bar.c`): lit segments coloured along a gradient, the rest
//! in an empty colour. Pure cairo.

use crate::{
    config::{Color, gradient3},
    render::BoxRect,
};

/// Gap between segments, in px.
pub(crate) const SEGMENT_GAP: f64 = 1.0;

/// Draws a meter `fraction` full (clamped to 0..=1) in `rect`, split into
/// `segments`. Any non-zero fraction lights at least one segment.
pub(crate) fn draw(
    cr: &cairo::Context,
    rect: BoxRect,
    fraction: f64,
    segments: u32,
    gradient: &[Color; 3],
    empty: Color,
) -> Result<(), cairo::Error> {
    if segments == 0 || rect.w <= 0.0 {
        return Ok(());
    }
    let fraction = fraction.clamp(0.0, 1.0);
    let count = f64::from(segments);
    let mut lit = crate::render::whole_px((fraction * count).round());
    if lit == 0 && fraction > 0.0 {
        lit = 1;
    }
    let seg_w = (rect.w - SEGMENT_GAP * (count - 1.0)) / count;
    cr.save()?;
    for i in 0..segments {
        let color = if i32::try_from(i).is_ok_and(|i| i < lit) {
            let t = if segments > 1 {
                f64::from(i) / (count - 1.0)
            } else {
                0.0
            };
            gradient3(gradient, t)
        } else {
            empty
        };
        color.set_source(cr);
        cr.rectangle(
            rect.x + f64::from(i) * (seg_w + SEGMENT_GAP),
            rect.y,
            seg_w,
            rect.h,
        );
        cr.fill()?;
    }
    cr.restore()
}
