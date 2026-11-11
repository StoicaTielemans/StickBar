//! The look every dropdown shares (the C bar's `render/popup_chrome.c`): a
//! mantle surface with the module's accent as its border, and `base` cards
//! inside with a 1px `surface1` border, or a 2px accent border on the
//! "current" card (current month, default device, active player...). Plus
//! the header row every dropdown starts with: icon, bold title, dim summary,
//! and a filled status pill at the right end.

use super::{BoxRect, module_box::draw_rect, popup_style, text::TextCache};
use crate::config::{BAR, COLORS, Color, FONT};

/// What goes in a dropdown's header row. Empty strings / `None` are left out.
#[derive(Debug)]
pub(crate) struct Header<'a> {
    /// A Nerd Font glyph at the left.
    pub(crate) icon: Option<char>,
    /// The icon's colour (usually the dropdown's accent).
    pub(crate) icon_color: Color,
    /// Bold title, in `text`.
    pub(crate) title: &'a str,
    /// Dim text right after the title, in `subtext0`.
    pub(crate) summary: &'a str,
    /// The status pill's word, right-aligned.
    pub(crate) pill: Option<&'a str>,
    /// The pill's fill colour.
    pub(crate) pill_fill: Color,
}

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

/// A status pill's width for `word`.
pub(crate) fn pill_width(text: &mut TextCache, word: &str) -> f64 {
    text.width(word, FONT.xs, true) + popup_style::PILL_PAD_X * 2.0
}

/// A filled pill with `word` in it, ending at `right_x`, vertically centered in
/// the row `row_y..row_y + row_h`.
pub(crate) fn draw_pill(
    cr: &cairo::Context,
    text: &mut TextCache,
    (right_x, row_y, row_h): (f64, f64, f64),
    word: &str,
    fill: Color,
) -> Result<(), cairo::Error> {
    let w = pill_width(text, word);
    let rect = BoxRect {
        x: right_x - w,
        y: row_y + (row_h - popup_style::PILL_H) / 2.0,
        w,
        h: popup_style::PILL_H,
    };
    draw_rect(cr, rect, fill, COLORS.mantle, 0.0)?;
    text.draw_centered(cr, rect, word, FONT.xs, true, COLORS.mantle)
}

/// The header row, `w` wide at (x, y). The pill goes at the right end; the
/// title and summary are shortened with `...` rather than run into it.
pub(crate) fn draw_header(
    cr: &cairo::Context,
    text: &mut TextCache,
    (x, y, w): (f64, f64, f64),
    header: &Header<'_>,
) -> Result<(), cairo::Error> {
    let h = popup_style::HEADER_H;
    let mut right = x + w;
    if let Some(pill) = header.pill {
        draw_pill(cr, text, (right, y, h), pill, header.pill_fill)?;
        right -= pill_width(text, pill) + popup_style::COL_GAP;
    }
    let mut tx = x;
    if let Some(icon) = header.icon {
        let icon = icon.to_string();
        text.draw_left(
            cr,
            (tx, y, h),
            &icon,
            FONT.heading,
            false,
            header.icon_color,
        )?;
        tx += text.width(&icon, FONT.heading, false) + popup_style::HEADER_ICON_GAP;
    }
    if !header.title.is_empty() {
        let title = text.truncate(header.title, right - tx, FONT.md, true);
        text.draw_left(cr, (tx, y, h), &title, FONT.md, true, COLORS.text)?;
        tx += text.width(&title, FONT.md, true);
    }
    if !header.summary.is_empty() && right - tx > 0.0 {
        let summary = text.truncate(header.summary, right - tx, FONT.sm, false);
        text.draw_left(cr, (tx, y, h), &summary, FONT.sm, false, COLORS.subtext0)?;
    }
    Ok(())
}

/// The width a header needs to show everything unshortened, for sizing a
/// dropdown to its header.
pub(crate) fn header_width(text: &mut TextCache, header: &Header<'_>) -> f64 {
    let mut w = 0.0;
    if let Some(icon) = header.icon {
        w += text.width(&icon.to_string(), FONT.heading, false) + popup_style::HEADER_ICON_GAP;
    }
    if !header.title.is_empty() {
        w += text.width(header.title, FONT.md, true);
    }
    if !header.summary.is_empty() {
        w += text.width(header.summary, FONT.sm, false);
    }
    if let Some(pill) = header.pill {
        w += popup_style::PILL_GAP + pill_width(text, pill);
    }
    w
}
