//! The smart-plug card in the media dropdown (`docs/modules/shelly.md`),
//! under the EQ card. Only shown with a `shelly.conf`; nothing in the bar.
//!
//! - **Header:** plug glyph, `Smart plugs  ·  2/3 on`, and a pill: the total
//!   watts of the plugs that are on (green), `all off`, `offline` (nothing
//!   answers) or `…` (not asked yet), grey.
//! - **One row per plug:** a state dot, the name, the live watts while it's
//!   on (or `offline` / the error in its colour), and a badge: `ON` green,
//!   `off` grey, `…` yellow while a switch is under way, `--` with no answer.
//!   **The whole row toggles the plug**; it lights up on hover, but only when
//!   the plug's state is known and nothing is pending.
//! - **Chips:** `All on` and `All off`, which switch every plug.
//!
//! The card's size depends only on the plug count (fixed for the bar's run),
//! so a state change only repaints: it never resizes the dropdown.

use crate::{
    config::{COLORS, Color, FONT, glyphs::ICON_PLUG},
    render::{
        BoxRect,
        module_box::draw_rect,
        popup_chrome::{self, Header},
        popup_style::HEADER_H,
        text::TextCache,
    },
    shelly::{Plug, PlugState, Shelly},
};

// The dropdown section, local to it (CLAUDE.md "Conventions").
/// Card edge to its content: the other cards' padding, so the columns line up.
const CARD_PAD: f64 = 10.0;
/// Header row to the first plug row.
const HEADER_GAP: f64 = 6.0;
/// One plug row: also its click area.
const ROW_H: f64 = 26.0;
/// Between plug rows.
const ROW_GAP: f64 = 4.0;
/// The last row to the chips.
const CHIPS_GAP: f64 = 8.0;
/// A chip's height (the EQ's chips' height).
const CHIP_H: f64 = 24.0;
/// A chip's text to its edge, each side.
const CHIP_PAD_X: f64 = 12.0;
/// Between the chips.
const CHIP_GAP: f64 = 6.0;
/// The state dot's radius.
const DOT_R: f64 = 4.0;
/// Row edge to the dot's centre.
const DOT_X: f64 = 12.0;
/// Row edge to the name.
const NAME_X: f64 = 24.0;
/// The `ON`/`off` badge's size.
const BADGE_W: f64 = 40.0;
/// The badge's height.
const BADGE_H: f64 = 18.0;
/// The badge to the row's right edge.
const BADGE_INSET: f64 = 4.0;
/// Between the watts (or status text) and the badge, and the name and watts.
const TEXT_GAP: f64 = 10.0;
/// The chips' and badges' font size (bold).
const CHIP_FONT: i32 = FONT.sm;
/// The card's own accent: hover and the "on" colour.
const ACCENT: Color = COLORS.green;
/// The chips' labels, left to right, and whether each switches on.
const CHIPS: [(&str, bool); 2] = [("All on", true), ("All off", false)];
/// `…`, for "not known yet" and "switching".
const ELLIPSIS: &str = "\u{2026}";

/// What a point on the card hits.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ShellyHit {
    /// Plug row `i`.
    Row(usize),
    /// Chip `i` of [`CHIPS`] (`All on`, `All off`). An index, not the bool
    /// it stands for, so both variants are the same size.
    Chip(usize),
}

/// The smart-plug card: where it is, and where its rows and chips are.
#[derive(Clone, Debug)]
pub(crate) struct ShellySection {
    /// The card's left edge and width (the EQ card's column).
    card: (f64, f64),
    /// The card's top (set by [`Self::place`]).
    y: f64,
    /// How many plug rows.
    rows: usize,
    /// The two chips' widths, measured once when the dropdown opens.
    chip_w: [f64; 2],
}

impl ShellySection {
    /// A card `card_w` wide at `card_x`, with a row for each of `rows` plugs.
    pub(crate) fn new(text: &mut TextCache, (card_x, card_w): (f64, f64), rows: usize) -> Self {
        let chip_w = CHIPS.map(|(label, _)| text.width(label, CHIP_FONT, true) + CHIP_PAD_X * 2.0);
        Self {
            card: (card_x, card_w),
            y: 0.0,
            rows,
            chip_w,
        }
    }

    /// How many plug rows it has.
    pub(crate) const fn rows(&self) -> usize {
        self.rows
    }

    /// Puts the card's top at `y` (popup-local).
    pub(crate) const fn place(&mut self, y: f64) {
        self.y = y;
    }

    /// The rows' total height, gaps included.
    fn rows_h(&self) -> f64 {
        let gaps = self.rows.saturating_sub(1);
        // Both counts are at most config::MAX_PLUGS (12): exact in u32.
        let n = f64::from(u32::try_from(self.rows).unwrap_or(0));
        let g = f64::from(u32::try_from(gaps).unwrap_or(0));
        n * ROW_H + g * ROW_GAP
    }

    /// The card's height.
    pub(crate) fn height(&self) -> f64 {
        CARD_PAD + HEADER_H + HEADER_GAP + self.rows_h() + CHIPS_GAP + CHIP_H + CARD_PAD
    }

    /// The content's left edge and width.
    const fn inner(&self) -> (f64, f64) {
        (self.card.0 + CARD_PAD, self.card.1 - CARD_PAD * 2.0)
    }

    /// Plug row `i`'s rect, popup-local.
    fn row(&self, i: usize) -> Option<BoxRect> {
        let step = f64::from(u32::try_from(i).ok()?);
        (i < self.rows).then(|| {
            let (x, w) = self.inner();
            BoxRect {
                x,
                y: self.y + CARD_PAD + HEADER_H + HEADER_GAP + step * (ROW_H + ROW_GAP),
                w,
                h: ROW_H,
            }
        })
    }

    /// Chip `i`'s rect ([`CHIPS`] order, left to right), popup-local.
    fn chip(&self, i: usize) -> Option<BoxRect> {
        let w = *self.chip_w.get(i)?;
        let before: f64 = self.chip_w.iter().take(i).map(|left| left + CHIP_GAP).sum();
        Some(BoxRect {
            x: self.inner().0 + before,
            y: self.y + self.height() - CARD_PAD - CHIP_H,
            w,
            h: CHIP_H,
        })
    }

    /// What's under (x, y), if anything (not the gaps).
    pub(crate) fn hit(&self, x: f64, y: f64) -> Option<ShellyHit> {
        if let Some(i) = (0..self.rows).find(|&i| self.row(i).is_some_and(|r| r.contains(x, y))) {
            return Some(ShellyHit::Row(i));
        }
        (0..CHIPS.len())
            .find(|&i| self.chip(i).is_some_and(|r| r.contains(x, y)))
            .map(ShellyHit::Chip)
    }

    /// Where the tests' `target shelly <what>` points, popup-local: the
    /// middle of a plug's row (`NAME`) or of a chip (`all-on`, `all-off`).
    pub(crate) fn target(&self, what: &str, shelly: &Shelly) -> Option<(f64, f64)> {
        let rect = match what {
            "all-on" => self.chip(0)?,
            "all-off" => self.chip(1)?,
            name => self.row(shelly.plugs.iter().position(|p| p.name == name)?)?,
        };
        Some((rect.x + rect.w / 2.0, rect.y + rect.h / 2.0))
    }

    /// Draws the card. `hovered` is what's under the pointer.
    pub(crate) fn draw(
        &self,
        cr: &cairo::Context,
        text: &mut TextCache,
        shelly: &Shelly,
        hovered: Option<ShellyHit>,
    ) -> Result<(), cairo::Error> {
        let (card_x, card_w) = self.card;
        let card = BoxRect {
            x: card_x,
            y: self.y,
            w: card_w,
            h: self.height(),
        };
        popup_chrome::draw_card(cr, card, false, false, ACCENT)?;
        let (inner_x, inner_w) = self.inner();
        draw_header(cr, text, shelly, (inner_x, self.y + CARD_PAD, inner_w))?;
        for (i, plug) in shelly.plugs.iter().enumerate() {
            if let Some(rect) = self.row(i) {
                let lit = hovered == Some(ShellyHit::Row(i)) && toggle_target(plug).is_some();
                draw_row(cr, text, plug, rect, lit)?;
            }
        }
        for (i, (label, on)) in CHIPS.into_iter().enumerate() {
            let Some(rect) = self.chip(i) else {
                continue;
            };
            let lit = hovered == Some(ShellyHit::Chip(i));
            popup_chrome::draw_card(cr, rect, false, lit, ACCENT)?;
            let color = if on { ACCENT } else { COLORS.text };
            text.draw_centered(cr, rect, label, CHIP_FONT, true, color)?;
        }
        Ok(())
    }
}

/// What chip `i` switches every plug to (`true`: on).
pub(crate) fn chip_switches_on(i: usize) -> Option<bool> {
    CHIPS.get(i).map(|&(_, on)| on)
}

/// What a click on `plug`'s row switches it to: the opposite of what it
/// shows. `None` (not clickable) while its state is unknown or a switch is
/// under way. A `Set`, not a `Toggle`, so two quick clicks can't flip it
/// twice behind a stale view.
pub(crate) fn toggle_target(plug: &Plug) -> Option<bool> {
    if plug.pending {
        return None;
    }
    match plug.state {
        PlugState::On => Some(false),
        PlugState::Off => Some(true),
        PlugState::Unknown | PlugState::Offline | PlugState::Error(_) => None,
    }
}

/// `42 W`, or `3.4 W` under 10 W (a phone charger's draw would read `3 W`).
fn format_watts(watts: f64) -> String {
    if watts < 10.0 {
        format!("{watts:.1} W")
    } else {
        format!("{watts:.0} W")
    }
}

/// The header row: the pill sums what's on.
fn draw_header(
    cr: &cairo::Context,
    text: &mut TextCache,
    shelly: &Shelly,
    row: (f64, f64, f64),
) -> Result<(), cairo::Error> {
    let on = shelly.on_count();
    let summary = format!("  \u{b7}  {on}/{} on", shelly.plugs.len());
    let any = |state: &PlugState| shelly.plugs.iter().any(|p| &p.state == state);
    let pill = if on > 0 {
        (format_watts(shelly.total_watts()), ACCENT)
    } else if any(&PlugState::Off) {
        ("all off".to_owned(), COLORS.overlay1)
    } else if shelly.plugs.iter().all(|p| p.state == PlugState::Unknown) {
        (ELLIPSIS.to_owned(), COLORS.overlay1)
    } else {
        ("offline".to_owned(), COLORS.overlay1)
    };
    let header = Header {
        icon: Some(ICON_PLUG),
        icon_color: ACCENT,
        title: "Smart plugs",
        summary: &summary,
        pill: Some(&pill.0),
        pill_fill: pill.1,
    };
    popup_chrome::draw_header(cr, text, row, &header)
}

/// One plug's row. `lit`: hovered and clickable.
fn draw_row(
    cr: &cairo::Context,
    text: &mut TextCache,
    plug: &Plug,
    rect: BoxRect,
    lit: bool,
) -> Result<(), cairo::Error> {
    popup_chrome::draw_card(cr, rect, false, lit, ACCENT)?;
    let (dot, detail, detail_color) = match &plug.state {
        PlugState::On => (
            ACCENT,
            plug.watts.map(format_watts).unwrap_or_default(),
            COLORS.subtext0,
        ),
        PlugState::Off => (COLORS.overlay1, String::new(), COLORS.subtext0),
        PlugState::Offline => (COLORS.overlay0, "offline".to_owned(), COLORS.overlay0),
        PlugState::Error(why) => (COLORS.red, why.clone(), COLORS.red),
        PlugState::Unknown => (COLORS.overlay0, String::new(), COLORS.overlay0),
    };
    dot.set_source(cr);
    cr.arc(
        rect.x + DOT_X,
        rect.y + rect.h / 2.0,
        DOT_R,
        0.0,
        std::f64::consts::TAU,
    );
    cr.fill()?;

    // The badge, at the right end.
    let badge = BoxRect {
        x: rect.x + rect.w - BADGE_INSET - BADGE_W,
        y: rect.y + (rect.h - BADGE_H) / 2.0,
        w: BADGE_W,
        h: BADGE_H,
    };
    let (word, fill, ink) = if plug.pending {
        (ELLIPSIS, COLORS.yellow, COLORS.mantle)
    } else {
        match plug.state {
            PlugState::On => ("ON", ACCENT, COLORS.mantle),
            PlugState::Off => ("off", COLORS.surface1, COLORS.subtext0),
            PlugState::Unknown | PlugState::Offline | PlugState::Error(_) => {
                ("--", COLORS.surface0, COLORS.overlay0)
            }
        }
    };
    draw_rect(cr, badge, fill, COLORS.mantle, 0.0)?;
    text.draw_centered(cr, badge, word, CHIP_FONT, true, ink)?;

    // The watts (or why not), right-aligned before the badge; the name gets
    // what's left.
    let detail_right = badge.x - TEXT_GAP;
    let name_x = rect.x + NAME_X;
    let detail = text.truncate(&detail, (detail_right - name_x) / 2.0, FONT.sm, false);
    let detail_w = text.width(&detail, FONT.sm, false);
    if !detail.is_empty() {
        text.draw_right(
            cr,
            (detail_right, rect.y, rect.h),
            &detail,
            FONT.sm,
            false,
            detail_color,
        )?;
    }
    let name_w = detail_right - name_x - detail_w - TEXT_GAP;
    let name = text.truncate(&plug.name, name_w, FONT.md, true);
    text.draw_left(
        cr,
        (name_x, rect.y, rect.h),
        &name,
        FONT.md,
        true,
        COLORS.text,
    )
}

#[cfg(test)]
mod tests {
    use super::format_watts;

    #[test]
    fn watts_keep_a_decimal_only_when_small() {
        assert_eq!(format_watts(3.44), "3.4 W");
        assert_eq!(format_watts(42.5), "42 W");
        assert_eq!(format_watts(1234.0), "1234 W");
    }
}
