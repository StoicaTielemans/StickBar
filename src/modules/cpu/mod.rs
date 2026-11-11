//! The CPU box, e.g. `󰻠 12% 45°C` in peach (the C bar's `modules/cpu/cpu.c`).
//! A right click opens its dropdown (`popup.rs`).

pub(crate) mod popup;

use crate::{
    config::{COLORS, glyphs::ICON_CPU},
    render::{
        BoxRect,
        module_box::{self, Anchor},
        text::TextCache,
        whole_px,
    },
    sysmon::cpu::Cpu,
};

/// Draws the box starting at `x` (left region), spanning `top..top + height`.
pub(crate) fn render(
    cr: &cairo::Context,
    text: &mut TextCache,
    cpu: &Cpu,
    x: f64,
    (top, height): (f64, f64),
) -> Result<BoxRect, cairo::Error> {
    let label = format!(
        "{ICON_CPU} {}% {}\u{b0}C",
        whole_px(cpu.percent.round()),
        cpu.temp_c
    );
    module_box::label(
        cr,
        text,
        (Anchor::FromLeft, x),
        (top, height),
        &label,
        COLORS.peach,
    )
}
