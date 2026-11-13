//! The GPU box, e.g. `󰢮 3% 41°C` in green, or `󰢮 --` without an NVIDIA GPU
//! (the C bar's `modules/gpu/gpu.c`). A right click opens its dropdown.

pub(crate) mod popup;

use crate::{
    config::{COLORS, glyphs::ICON_GPU},
    render::{
        BoxRect,
        module_box::{self, Anchor},
        text::TextCache,
    },
    sysmon::gpu::Gpu,
};

/// Draws the box starting at `x` (left region), spanning `top..top + height`.
pub(crate) fn render(
    cr: &cairo::Context,
    text: &mut TextCache,
    gpu: &Gpu,
    x: f64,
    (top, height): (f64, f64),
) -> Result<BoxRect, cairo::Error> {
    let label = if gpu.available {
        format!("{ICON_GPU} {}% {}\u{b0}C", gpu.percent, gpu.temp_c)
    } else {
        // An explicit "unavailable", never a misleading 0%.
        format!("{ICON_GPU} --")
    };
    module_box::label(
        cr,
        text,
        (Anchor::FromLeft, x),
        (top, height),
        &label,
        COLORS.green,
    )
}
