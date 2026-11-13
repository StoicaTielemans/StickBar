//! The memory box, e.g. `󰍛 41%` in yellow (the C bar's `modules/mem/mem.c`).
//! A right click opens its dropdown (`popup.rs`); a left click does nothing,
//! since a status box has no obvious primary action (`docs/popups.md`).

pub(crate) mod popup;

use crate::{
    config::{COLORS, glyphs::ICON_MEM},
    render::{
        BoxRect,
        module_box::{self, Anchor},
        text::TextCache,
    },
    sysmon::memory::Memory,
};

/// Draws the box starting at `x` (left region), spanning `top..top + height`.
pub(crate) fn render(
    cr: &cairo::Context,
    text: &mut TextCache,
    mem: &Memory,
    x: f64,
    (top, height): (f64, f64),
) -> Result<BoxRect, cairo::Error> {
    let label = format!("{ICON_MEM} {}%", mem.percent);
    module_box::label(
        cr,
        text,
        (Anchor::FromLeft, x),
        (top, height),
        &label,
        COLORS.yellow,
    )
}
