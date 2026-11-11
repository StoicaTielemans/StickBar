//! The look every dropdown shares (the C bar's `render/popup_chrome.c`): a
//! mantle surface with the module's accent as its border, and `base` cards
//! inside with a 1px `surface1` border, or a 2px accent border on the
//! "current" card (current month, default device, active player...). The
//! header and status-pill helpers arrive with their first users.

use super::{BoxRect, module_box::draw_rect};
use crate::config::{BAR, COLORS, Color};

/// The popup's own background and border, filling the whole `w` x `h` surface.
pub(crate) fn draw_frame(
    cr: &cairo::Context,
    w: f64,
    h: f64,
    accent: Color,
) -> Result<(), cairo::Error> {
    let rect = BoxRect {
        x: 0.0,
        y: 0.0,
        w,
        h,
    };
    draw_rect(cr, rect, COLORS.mantle, accent, BAR.module_border_width)
}

/// One card. `current` marks it with the accent border; `hovered` lightens it.
pub(crate) fn draw_card(
    cr: &cairo::Context,
    rect: BoxRect,
    current: bool,
    hovered: bool,
    accent: Color,
) -> Result<(), cairo::Error> {
    let fill = if hovered {
        COLORS.surface0
    } else {
        COLORS.base
    };
    let (border, width) = if current {
        (accent, 2.0)
    } else {
        (COLORS.surface1, 1.0)
    };
    draw_rect(cr, rect, fill, border, width)
}
