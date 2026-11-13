//! The volume box: the default sink's level, e.g. `󰕾 45%` in sky (the C bar's
//! `modules/volume/volume.c`). Left click mutes, scrolling steps ±5 %; the
//! right-click device dropdown and the middle-click mixer arrive in M9b/M9c.

use crate::{
    audio::{AudioState, Kind, volume_style::level_glyph},
    config::COLORS,
    render::{
        BoxRect,
        module_box::{self, Anchor},
        text::TextCache,
    },
};

/// Draws the box ending at `right_x` (right region), spanning `top..top + height`.
pub(crate) fn render(
    cr: &cairo::Context,
    text: &mut TextCache,
    audio: &AudioState,
    right_x: f64,
    (top, height): (f64, f64),
) -> Result<BoxRect, cairo::Error> {
    let (percent, muted) = audio
        .default_node(Kind::Sink)
        .map_or((0, false), |n| (n.volume, n.muted));
    let label = format!("{} {percent}%", level_glyph(percent, muted));
    module_box::label(
        cr,
        text,
        (Anchor::FromRight, right_x),
        (top, height),
        &label,
        COLORS.sky,
    )
}
