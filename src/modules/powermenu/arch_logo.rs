//! The Arch logo, the bar's leftmost item and the power menu's button (the C
//! bar's `modules/powermenu/arch_logo.c`).
//!
//! Deliberately not the usual bordered module box: a solid sapphire block with
//! the logo drawn in mantle (the bar's own background colour), so it reads as
//! a shape cut out of the block rather than coloured text on a dark box.

use crate::{
    config::{COLORS, Color, FONT, glyphs::ICON_ARCH},
    render::{BoxRect, module_box::draw_rect, text::TextCache},
};

/// Extra width beyond the glyph's ink, split evenly on both sides.
const PAD: f64 = 12.0;

/// Draws the logo box starting at `x`, spanning `top..top + height`, and
/// returns where it landed.
pub(crate) fn render(
    cr: &cairo::Context,
    text: &mut TextCache,
    x: f64,
    (top, height): (f64, f64),
) -> Result<BoxRect, cairo::Error> {
    let glyph = ICON_ARCH.to_string();
    // Sized from the glyph's *ink*, not its advance width: this glyph draws
    // 14px of ink on an 8px advance, and the box used to cut it off (BUG.md,
    // "Arch logo cut off"). Centering still uses the advance, which the wider
    // box has room for.
    let rect = BoxRect {
        x,
        y: top,
        w: text.ink_width(&glyph, FONT.label, true) + PAD,
        h: height,
    };
    draw_rect(cr, rect, COLORS.sapphire, Color::TRANSPARENT, 0.0)?;
    text.draw_centered(cr, rect, &glyph, FONT.label, true, COLORS.mantle)?;
    Ok(rect)
}
