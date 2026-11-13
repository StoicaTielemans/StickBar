//! The DAC EQ inside the media module (`docs/modules/eq.md`). It used to be a
//! box and dropdown of its own; at the user's request it now lives only in the
//! media dropdown, with nothing in the bar:
//!
//! a card left of the player cards ([`EqSection`]): a header row (equalizer
//! glyph, profile name, `· low gain`, and a pill: `on` green, `off` /
//! `no data` grey, `applying` yellow), the preamp and peak/min, the curve on a
//! dB/frequency grid, and one compact chip per offered profile. Clicking a
//! chip runs the script; the dropdown stays open, the chip turns yellow until
//! the state write arrives, then the lavender border moves to it. While a
//! switch is under way no chip reacts.
//!
//! The section's size never depends on the state, so a state change only
//! repaints the dropdown: it never resizes.

use crate::{
    config::{COLORS, Color, FONT, glyphs::ICON_EQUALIZER},
    eq::{EqState, curve},
    render::{
        BoxRect,
        popup_chrome::{self, Header},
        popup_style::{HEADER_H, PILL_PAD_X},
        text::TextCache,
    },
};

/// The profiles offered, in display order: exactly the subset of
/// `DacController`'s presets the user asked for. Each name is passed straight
/// to `blackpearl-eq.py profile NAME`, so it must match a `presets/NAME.txt`:
/// - `zero2`: Zero:2 IEM, low gain;
/// - `dt990`: DT 990, full EQ, more bass;
/// - `dt990-loud`: DT 990, treble fix, louder;
/// - `flat-high` / `flat-low`: a flat EQ at each gain mode (presets with no
///   bands, so `profile` also sets the gain, unlike the script's `flat`).
pub(crate) const PROFILES: [&str; 5] = ["zero2", "dt990", "dt990-loud", "flat-high", "flat-low"];

// The dropdown section, local to it (CLAUDE.md "Conventions").
/// Card edge to its content: the player cards' padding, so the columns line up.
const CARD_PAD: f64 = 10.0;
/// Header row to the label row.
const HEADER_GAP: f64 = 6.0;
/// The preamp / peak-min label row.
const GRAPH_LABEL_H: f64 = 14.0;
/// Label row to graph: room for the top dB label, centred on the graph's top edge.
const GRAPH_TOP_GAP: f64 = 8.0;
/// The graph's height: shorter than the old dropdown's 110 px, since it now
/// shares a dropdown with the player cards.
const GRAPH_H: f64 = 90.0;
/// The left column for the `+6`/`-6` grid labels.
const DB_LABEL_W: f64 = 24.0;
/// Graph bottom edge to the Hz labels.
const FREQ_LABEL_GAP: f64 = 2.0;
/// An axis label's row height.
const AXIS_LABEL_H: f64 = 12.0;
/// The Hz labels to the first row of chips.
const CHIPS_GAP: f64 = 10.0;
/// A chip's height.
const CHIP_H: f64 = 24.0;
/// A chip's text to its edge, each side.
const CHIP_PAD_X: f64 = 9.0;
/// Between chips, and between chip rows.
const CHIP_GAP: f64 = 6.0;
/// The graph's dB span: at least ±12 dB, growing in grid steps to fit heavy
/// EQs (the bass-v preset's +19 dB) without clipping them.
const GRAPH_MIN_RANGE_DB: f64 = 12.0;
/// One dB grid step.
const GRAPH_STEP_DB: f64 = 6.0;
/// The frequency grid lines and their labels.
const DECADES: [(f64, &str); 3] = [(100.0, "100"), (1000.0, "1k"), (10_000.0, "10k")];
/// The chips' font size (bold).
const CHIP_FONT: i32 = FONT.sm;

/// The EQ card in the media dropdown: where it is, and where its chips are.
#[derive(Clone, Debug)]
pub(crate) struct EqSection {
    /// The card's left edge and width (the player cards' column).
    card: (f64, f64),
    /// The card's top (set by [`Self::place`]).
    y: f64,
    /// One rect per [`PROFILES`] entry: x absolute, y relative to the first
    /// chip row's top. Measured once, when the dropdown opens.
    chips: Vec<BoxRect>,
}

impl EqSection {
    /// Lays the chips out left to right in a card `card_w` wide at `card_x`,
    /// wrapping to a new row when one doesn't fit.
    pub(crate) fn new(text: &mut TextCache, (card_x, card_w): (f64, f64)) -> Self {
        let widths: Vec<f64> = PROFILES
            .iter()
            .map(|name| text.width(name, CHIP_FONT, true) + CHIP_PAD_X * 2.0)
            .collect();
        Self {
            card: (card_x, card_w),
            y: 0.0,
            chips: chip_rows(&widths, card_x + CARD_PAD, card_w - CARD_PAD * 2.0),
        }
    }

    /// Puts the card's top at `y` (popup-local).
    pub(crate) const fn place(&mut self, y: f64) {
        self.y = y;
    }

    /// The card's height.
    pub(crate) fn height(&self) -> f64 {
        let chips_h = self.chips.iter().map(|c| c.y + c.h).fold(0.0, f64::max);
        CARD_PAD
            + HEADER_H
            + HEADER_GAP
            + GRAPH_LABEL_H
            + GRAPH_TOP_GAP
            + GRAPH_H
            + FREQ_LABEL_GAP
            + AXIS_LABEL_H
            + CHIPS_GAP
            + chips_h
            + CARD_PAD
    }

    /// The first chip row's top.
    fn chips_y(&self) -> f64 {
        self.y + self.height() - CARD_PAD - self.chips.iter().map(|c| c.y + c.h).fold(0.0, f64::max)
    }

    /// Chip `i`'s rect, popup-local.
    fn chip(&self, i: usize) -> Option<BoxRect> {
        let chips_y = self.chips_y();
        self.chips.get(i).map(|c| BoxRect {
            y: chips_y + c.y,
            ..*c
        })
    }

    /// The chip under (x, y), if any (not the gaps between them).
    pub(crate) fn chip_at(&self, x: f64, y: f64) -> Option<usize> {
        (0..self.chips.len()).find(|&i| self.chip(i).is_some_and(|r| r.contains(x, y)))
    }

    /// Where the tests' `target eq <what>` points, popup-local: the middle of
    /// a chip (`NAME`), its left border (`NAME:edge`, for reading the border
    /// colour), or a pixel inside the pill's right padding (`pill`, never a
    /// pixel of its text).
    pub(crate) fn target(&self, what: &str) -> Option<(f64, f64)> {
        let (card_x, card_w) = self.card;
        if what == "pill" {
            return Some((
                card_x + card_w - CARD_PAD - PILL_PAD_X / 2.0,
                self.y + CARD_PAD + HEADER_H / 2.0,
            ));
        }
        let (name, edge) = what
            .strip_suffix(":edge")
            .map_or((what, false), |name| (name, true));
        let i = PROFILES.iter().position(|p| *p == name)?;
        let rect = self.chip(i)?;
        let x = if edge { rect.x } else { rect.x + rect.w / 2.0 };
        Some((x, rect.y + rect.h / 2.0))
    }

    /// Draws the card. `hovered` is the chip under the pointer.
    pub(crate) fn draw(
        &self,
        cr: &cairo::Context,
        text: &mut TextCache,
        eq: &EqState,
        hovered: Option<usize>,
    ) -> Result<(), cairo::Error> {
        let (card_x, card_w) = self.card;
        let card = BoxRect {
            x: card_x,
            y: self.y,
            w: card_w,
            h: self.height(),
        };
        popup_chrome::draw_card(cr, card, false, false, COLORS.lavender)?;
        let inner = (card_x + CARD_PAD, card_w - CARD_PAD * 2.0);
        let mut y = self.y + CARD_PAD;
        draw_header(cr, text, eq, (inner.0, y, inner.1))?;
        y += HEADER_H + HEADER_GAP;
        draw_graph(cr, text, eq, inner, y)?;
        for i in 0..PROFILES.len() {
            if let (Some(rect), Some(name)) = (self.chip(i), PROFILES.get(i)) {
                draw_chip(cr, text, eq, (rect, name), hovered == Some(i))?;
            }
        }
        Ok(())
    }
}

/// Places chips `widths` wide in rows within `x..x + w`, starting a new row
/// when the next chip wouldn't fit (a chip wider than the row gets one alone).
/// Rects are x-absolute, y relative to the first row's top.
fn chip_rows(widths: &[f64], x: f64, w: f64) -> Vec<BoxRect> {
    let mut rects = Vec::with_capacity(widths.len());
    let (mut cx, mut cy) = (x, 0.0);
    for &cw in widths {
        if cx > x && cx + cw > x + w {
            cx = x;
            cy += CHIP_H + CHIP_GAP;
        }
        rects.push(BoxRect {
            x: cx,
            y: cy,
            w: cw,
            h: CHIP_H,
        });
        cx += cw + CHIP_GAP;
    }
    rects
}

/// The pill: whether the EQ is doing anything right now. Applying comes
/// first, since that's what the user just asked for.
fn pill(eq: &EqState) -> (&'static str, Color) {
    if eq.pending.is_some() {
        ("applying", COLORS.yellow)
    } else if !eq.snap.available {
        ("no data", COLORS.overlay1)
    } else if !eq.snap.enabled {
        ("off", COLORS.overlay1)
    } else {
        ("on", COLORS.green)
    }
}

/// The header row, in the dropdown headers' language. A long preset name is
/// shortened short of the pill.
fn draw_header(
    cr: &cairo::Context,
    text: &mut TextCache,
    eq: &EqState,
    row: (f64, f64, f64),
) -> Result<(), cairo::Error> {
    let snap = &eq.snap;
    let title = if !snap.available {
        "No EQ state yet"
    } else if snap.profile.is_empty() {
        "custom EQ"
    } else {
        &snap.profile
    };
    let summary = if snap.gain.is_empty() {
        String::new()
    } else {
        format!("  \u{b7}  {} gain", snap.gain)
    };
    let (word, fill) = pill(eq);
    let header = Header {
        icon: Some(ICON_EQUALIZER),
        icon_color: COLORS.lavender,
        title,
        summary: &summary,
        pill: Some(word),
        pill_fill: fill,
    };
    popup_chrome::draw_header(cr, text, row, &header)
}

/// A 1 px line in `color`.
fn stroke_line(
    cr: &cairo::Context,
    (x0, y0): (f64, f64),
    (x1, y1): (f64, f64),
    color: Color,
) -> Result<(), cairo::Error> {
    color.set_source(cr);
    cr.set_line_width(1.0);
    cr.move_to(x0, y0);
    cr.line_to(x1, y1);
    cr.stroke()
}

/// The label row (preamp, peak/min) at `top`, then the dB grid every
/// [`GRAPH_STEP_DB`] with labels on the left, the decade frequency lines with
/// labels below, and the curve, all in the column `inner_x..inner_x + inner_w`.
fn draw_graph(
    cr: &cairo::Context,
    text: &mut TextCache,
    eq: &EqState,
    (inner_x, inner_w): (f64, f64),
    top: f64,
) -> Result<(), cairo::Error> {
    let snap = &eq.snap;
    if snap.available {
        let preamp = format!("preamp {:+.1} dB", snap.preamp);
        let extent = format!("peak {:+.1}  min {:+.1} dB", snap.peak_db, snap.min_db);
        text.draw_left(
            cr,
            (inner_x, top, GRAPH_LABEL_H),
            &preamp,
            FONT.sm,
            false,
            COLORS.subtext0,
        )?;
        text.draw_right(
            cr,
            (inner_x + inner_w, top, GRAPH_LABEL_H),
            &extent,
            FONT.xs,
            false,
            COLORS.overlay0,
        )?;
    } else {
        text.draw_left(
            cr,
            (inner_x, top, GRAPH_LABEL_H),
            "run blackpearl-eq.py state",
            FONT.sm,
            false,
            COLORS.subtext0,
        )?;
    }

    let graph = BoxRect {
        x: inner_x + DB_LABEL_W,
        y: top + GRAPH_LABEL_H + GRAPH_TOP_GAP,
        w: inner_w - DB_LABEL_W,
        h: GRAPH_H,
    };
    let range = curve::range_db(snap, GRAPH_MIN_RANGE_DB, GRAPH_STEP_DB);
    let mid_y = graph.y + graph.h / 2.0;
    let px_per_db = (graph.h / 2.0) / range;

    // The dB grid: a line per step (none at 0 dB: the curve draws a brighter
    // one), each labelled on the left.
    let mut db = -range;
    while db <= range + 0.001 {
        // +0.5: 1 px lines land on a pixel row (see curve.rs's zero line).
        let y = (mid_y - db * px_per_db).floor() + 0.5;
        let zero = db.abs() < 0.001;
        if !zero {
            stroke_line(cr, (graph.x, y), (graph.x + graph.w, y), COLORS.surface0)?;
        }
        let label = if zero {
            "0".to_owned()
        } else {
            format!("{db:+.0}")
        };
        text.draw_right(
            cr,
            (graph.x - 4.0, y - AXIS_LABEL_H / 2.0, AXIS_LABEL_H),
            &label,
            FONT.xs,
            false,
            COLORS.overlay0,
        )?;
        db += GRAPH_STEP_DB;
    }

    for (freq, label) in DECADES {
        let x = curve::freq_to_x(freq, graph.x, graph.w).floor() + 0.5;
        stroke_line(cr, (x, graph.y), (x, graph.y + graph.h), COLORS.surface0)?;
        let label_w = text.width(label, FONT.xs, false);
        text.draw_left(
            cr,
            (
                x - label_w / 2.0,
                graph.y + graph.h + FREQ_LABEL_GAP,
                AXIS_LABEL_H,
            ),
            label,
            FONT.xs,
            false,
            COLORS.overlay0,
        )?;
    }

    let line = if snap.enabled {
        COLORS.lavender
    } else {
        COLORS.overlay0
    };
    curve::draw(cr, snap, graph, range, (line, 2.0), Some(COLORS.surface2))
}

/// One profile chip, in the cards' language: the active one has the lavender
/// border and name, the one being applied the yellow ones, and a hovered one
/// lightens to show it's clickable. While a switch is under way nothing is
/// clickable, so nothing lights up.
fn draw_chip(
    cr: &cairo::Context,
    text: &mut TextCache,
    eq: &EqState,
    (rect, name): (BoxRect, &str),
    hovered: bool,
) -> Result<(), cairo::Error> {
    let pending = eq.pending.as_deref() == Some(name);
    let active = !pending && eq.snap.available && eq.snap.profile == name;
    let accent = if pending {
        COLORS.yellow
    } else {
        COLORS.lavender
    };
    let hovered = hovered && !eq.busy();
    popup_chrome::draw_card(cr, rect, active || pending, hovered, accent)?;
    let color = if active || pending {
        accent
    } else {
        COLORS.text
    };
    text.draw_centered(cr, rect, name, CHIP_FONT, true, color)
}

#[cfg(test)]
mod tests {
    use super::{CHIP_GAP, CHIP_H, chip_rows};

    #[test]
    fn chips_fill_a_row_then_wrap() {
        // 3 x 50 px + 2 gaps = 162 fits in 170; the fourth wraps.
        let rects = chip_rows(&[50.0, 50.0, 50.0, 50.0], 10.0, 170.0);
        let rows: Vec<(f64, f64)> = rects.iter().map(|r| (r.x, r.y)).collect();
        let second_row_y = CHIP_H + CHIP_GAP;
        assert_eq!(
            rows,
            [(10.0, 0.0), (66.0, 0.0), (122.0, 0.0), (10.0, second_row_y)]
        );
    }

    #[test]
    fn a_chip_wider_than_the_row_gets_one_alone() {
        let rects = chip_rows(&[300.0, 20.0], 0.0, 100.0);
        assert!((rects[0].x).abs() < 1e-9 && (rects[0].y).abs() < 1e-9);
        assert!((rects[1].y - (CHIP_H + CHIP_GAP)).abs() < 1e-9);
    }
}
