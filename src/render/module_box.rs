//! The bordered "module box" every bar widget sits in (the box half of the C
//! bar's `render/module_box.c`).

use super::{BoxRect, text::TextCache};
use crate::config::{BAR, COLORS, Color, FONT};

/// Fills `rect` with `fill` and strokes a `border_width` border in `border`
/// just inside its edge. The shared box chrome every bar widget (and later
/// every popup card) uses; callers vary only colours, border and size.
pub(crate) fn draw_rect(
    cr: &cairo::Context,
    rect: BoxRect,
    fill: Color,
    border: Color,
    border_width: f64,
) -> Result<(), cairo::Error> {
    cr.save()?;
    if fill.a > 0.0 {
        cr.rectangle(rect.x, rect.y, rect.w, rect.h);
        fill.set_source(cr);
        cr.fill()?;
    }
    if border_width > 0.0 && border.a > 0.0 {
        // A stroke is centered on its path, so the path is inset by half the
        // width: the whole border then lands inside the box, none outside it.
        let inset = border_width / 2.0;
        cr.rectangle(
            rect.x + inset,
            rect.y + inset,
            rect.w - border_width,
            rect.h - border_width,
        );
        border.set_source(cr);
        cr.set_line_width(border_width);
        cr.stroke()?;
    }
    cr.restore()
}

/// Which edge a bar box is laid out from (the C bar's `ModuleBoxAnchor`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Anchor {
    /// Left-region boxes: the box starts at the given x and grows rightwards.
    FromLeft,
    /// Right-region boxes: the box ends at the given x and grows leftwards.
    FromRight,
}

/// The standard bar box (the C bar's `module_box_label`): `label` in bold at
/// the bar's label size, centered in a box `module_padding` wider than it, with
/// a mantle fill and an `accent` border, placed from `edge_x` as `anchor` says.
/// Returns where the box was drawn.
pub(crate) fn label(
    cr: &cairo::Context,
    text: &mut TextCache,
    (anchor, edge_x): (Anchor, f64),
    (top, height): (f64, f64),
    label: &str,
    accent: Color,
) -> Result<BoxRect, cairo::Error> {
    let w = text.width(label, FONT.label, true) + BAR.module_padding;
    let x = match anchor {
        Anchor::FromLeft => edge_x,
        Anchor::FromRight => edge_x - w,
    };
    let rect = BoxRect {
        x,
        y: top,
        w,
        h: height,
    };
    draw_rect(cr, rect, COLORS.mantle, accent, BAR.module_border_width)?;
    text.draw_centered(cr, rect, label, FONT.label, true, accent)?;
    Ok(rect)
}
