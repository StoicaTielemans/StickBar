//! The mic box: the default source's level, e.g. `󰍬 80%` in blue (the C bar's
//! `modules/mic/mic.c`). Left click mutes, scrolling steps ±5 %; the
//! right-click device dropdown arrives in M9b.

use crate::{
    audio::{AudioState, Kind},
    config::{
        COLORS,
        glyphs::{ICON_MIC, ICON_MIC_MUTED},
    },
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
        .default_node(Kind::Source)
        .map_or((0, false), |n| (n.volume, n.muted));
    let icon = if muted || percent == 0 {
        ICON_MIC_MUTED
    } else {
        ICON_MIC
    };
    let label = format!("{icon} {percent}%");
    module_box::label(
        cr,
        text,
        (Anchor::FromRight, right_x),
        (top, height),
        &label,
        COLORS.blue,
    )
}
