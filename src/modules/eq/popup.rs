//! The EQ dropdown (the C bar's `modules/eq/eq_popup.c`; `docs/modules/eq.md`
//! describes each part). Anchored under the box's right edge, like the other
//! right-region dropdowns.
//!
//! Top to bottom:
//! - header: equalizer glyph, profile name, `· low gain`, and a pill: `on`
//!   green, `off` / `no data` grey, `applying` yellow;
//! - a graph card: `preamp -3.0 dB` and the curve's peak/min, then the dB and
//!   frequency grid and the curve;
//! - one card per offered profile. Clicking one runs the script and leaves
//!   the dropdown open: the card says `applying...` until the state write
//!   arrives, then the accent border moves to it.
//!
//! Everything is a fixed size (nothing depends on the state's text), so a
//! state change only repaints: the popup never resizes.

use std::fs::File;

use smithay_client_toolkit::reexports::client::protocol::wl_output::WlOutput;

use crate::{
    bar::{
        popup_host::PopupHost,
        popup_surface::{PopupEnv, PopupSurface, render_image},
    },
    config::{COLORS, Color, FONT, glyphs::ICON_EQUALIZER},
    eq::{EqState, curve},
    render::{
        BoxRect,
        popup_chrome::{self, Header},
        popup_style::{COL_GAP, HEADER_H, OUTER_MARGIN, SECTION_GAP},
        text::TextCache,
        whole_px,
    },
};

/// A profile card: its name (passed straight to `blackpearl-eq.py profile
/// NAME`, so it must match a `DacController` `presets/NAME.txt`) and the
/// description shown on the right.
struct Profile {
    /// The preset name.
    name: &'static str,
    /// What it's for.
    description: &'static str,
}

/// The profiles offered, in display order: exactly the subset of
/// `DacController`'s presets the user asked for (the IEM profile, two DT 990
/// variants, and a flat EQ at each gain mode; `flat-high`/`flat-low` are
/// presets with no bands, so `profile` also sets the gain, unlike the
/// script's own `flat` command).
const PROFILES: [Profile; 5] = [
    Profile {
        name: "zero2",
        description: "Zero:2 IEM \u{b7} low gain",
    },
    Profile {
        name: "dt990",
        description: "DT 990 \u{b7} full EQ, more bass",
    },
    Profile {
        name: "dt990-loud",
        description: "DT 990 \u{b7} treble fix, louder",
    },
    Profile {
        name: "flat-high",
        description: "Flat EQ \u{b7} high gain",
    },
    Profile {
        name: "flat-low",
        description: "Flat EQ \u{b7} low gain",
    },
];

// Layout, local to this popup (CLAUDE.md "Conventions"); header, pill and card
// styling match the other dropdowns on purpose.
/// The content column's width.
const CONTENT_W: f64 = 340.0;
/// Card edge to its content.
const CARD_PAD: f64 = 8.0;
/// Between profile cards: tighter than `popup_style::CARD_GAP` (8), for five
/// cards in a fixed-size popup.
const CARD_GAP: f64 = 6.0;
/// The graph card's label row.
const GRAPH_LABEL_H: f64 = 14.0;
/// Label row to graph: room for the top dB label, centred on the graph's top edge.
const GRAPH_TOP_GAP: f64 = 8.0;
/// The graph's height.
const GRAPH_H: f64 = 110.0;
/// The left column for the `+6`/`-6` grid labels.
const DB_LABEL_W: f64 = 24.0;
/// Graph bottom edge to the Hz labels.
const FREQ_LABEL_GAP: f64 = 2.0;
/// An axis label's row height.
const AXIS_LABEL_H: f64 = 12.0;
/// The `PROFILES` label's row height.
const SECTION_LABEL_H: f64 = 14.0;
/// `PROFILES` label to the first card.
const SECTION_LABEL_GAP: f64 = 4.0;
/// A profile card's height.
const PROFILE_CARD_H: f64 = 30.0;
/// The graph's dB span: at least ±12 dB, growing in grid steps to fit heavy
/// EQs (the bass-v preset's +19 dB) without clipping them.
const GRAPH_MIN_RANGE_DB: f64 = 12.0;
/// One dB grid step.
const GRAPH_STEP_DB: f64 = 6.0;
/// The frequency grid lines and their labels.
const DECADES: [(f64, &str); 3] = [(100.0, "100"), (1000.0, "1k"), (10_000.0, "10k")];

// Derived geometry: a straight stack, top to bottom.
/// The popup's width.
const POPUP_W: f64 = CONTENT_W + OUTER_MARGIN * 2.0;
/// Where text inside a card starts.
const INNER_X: f64 = OUTER_MARGIN + CARD_PAD;
/// The width of a card's content.
const INNER_W: f64 = CONTENT_W - CARD_PAD * 2.0;
/// The graph card's top.
const GRAPH_CARD_Y: f64 = OUTER_MARGIN + HEADER_H + SECTION_GAP;
/// The graph card's height.
const GRAPH_CARD_H: f64 =
    CARD_PAD + GRAPH_LABEL_H + GRAPH_TOP_GAP + GRAPH_H + FREQ_LABEL_GAP + AXIS_LABEL_H + CARD_PAD;
/// The graph's left edge.
const GRAPH_X: f64 = INNER_X + DB_LABEL_W;
/// The graph's top edge.
const GRAPH_Y: f64 = GRAPH_CARD_Y + CARD_PAD + GRAPH_LABEL_H + GRAPH_TOP_GAP;
/// The graph's width.
const GRAPH_W: f64 = INNER_W - DB_LABEL_W;
/// The `PROFILES` label's top.
const SECTION_LABEL_Y: f64 = GRAPH_CARD_Y + GRAPH_CARD_H + SECTION_GAP;
/// The first profile card's top.
const PROFILES_Y: f64 = SECTION_LABEL_Y + SECTION_LABEL_H + SECTION_LABEL_GAP;
/// The popup's height: five cards, four gaps.
const POPUP_H: f64 = PROFILES_Y + 5.0 * PROFILE_CARD_H + 4.0 * CARD_GAP + OUTER_MARGIN;

/// The EQ dropdown: closed (`host` is `None`) or open.
#[derive(Default)]
pub(crate) struct EqPopup {
    /// The open popup's surfaces.
    host: Option<PopupHost>,
    /// The profile card under the pointer.
    hovered: Option<usize>,
}

impl EqPopup {
    /// Opens the dropdown under the box `trigger` on `output` (`output_width`
    /// px wide), re-opening it if it's open.
    pub(crate) fn open<'o>(
        &mut self,
        env: &PopupEnv<'_>,
        (output, output_width): (&WlOutput, i32),
        trigger: BoxRect,
        all_outputs: impl Iterator<Item = &'o WlOutput>,
        eq: &EqState,
        text: &mut TextCache,
    ) {
        self.host = None;
        self.hovered = None;
        self.host = Some(PopupHost::open_right(
            env,
            (output, output_width),
            trigger,
            (whole_px(POPUP_W), whole_px(POPUP_H)),
            all_outputs,
        ));
        self.paint(eq, text);
    }

    /// Closes the dropdown (no-op if closed).
    pub(crate) fn close(&mut self) {
        self.host = None;
        self.hovered = None;
    }

    /// The open popup's surfaces, if open.
    pub(crate) const fn host(&self) -> Option<&PopupHost> {
        self.host.as_ref()
    }

    /// The open popup's surfaces, for configure routing.
    pub(crate) const fn host_mut(&mut self) -> Option<&mut PopupHost> {
        self.host.as_mut()
    }

    /// The menu surface, if open.
    pub(crate) fn menu(&self) -> Option<&PopupSurface> {
        self.host.as_ref().map(PopupHost::menu)
    }

    /// How many profile cards are shown (the tests' `menu`), `None` when closed.
    pub(crate) fn card_count(&self) -> Option<usize> {
        self.host.as_ref().map(|_| PROFILES.len())
    }

    /// Paints the open popup (after a state change too: the size never changes).
    pub(crate) fn paint(&mut self, eq: &EqState, text: &mut TextCache) {
        let hovered = self.hovered;
        if let Some(host) = self.host.as_mut() {
            host.menu_mut().paint(|cr| draw(cr, text, eq, hovered));
        }
    }

    /// The pointer moved to (x, y) on the menu: the hover highlight follows.
    pub(crate) fn motion(&mut self, (x, y): (f64, f64), eq: &EqState, text: &mut TextCache) {
        let hovered = card_at(x, y);
        if hovered != self.hovered {
            self.hovered = hovered;
            self.paint(eq, text);
        }
    }

    /// A press at (x, y) on the menu: the profile to apply, if it landed on a
    /// card and no switch is already under way. The header and graph are
    /// read-only; the press is absorbed either way.
    pub(crate) fn press(&self, (x, y): (f64, f64), eq: &EqState) -> Option<&'static str> {
        if self.host.is_none() || eq.busy() {
            return None;
        }
        card_at(x, y).and_then(|i| PROFILES.get(i)).map(|p| p.name)
    }

    /// Whether a click at (x, y) on the menu would do something (the hand
    /// cursor): on a card, while no switch is under way.
    pub(crate) fn over_clickable(&self, (x, y): (f64, f64), eq: &EqState) -> bool {
        self.host.is_some() && !eq.busy() && card_at(x, y).is_some()
    }

    /// The middle of profile `name`'s card, menu-local (the tests' `target eq`).
    pub(crate) fn target(&self, name: &str) -> Option<(f64, f64)> {
        let _open = self.host.as_ref()?;
        let i = PROFILES.iter().position(|p| p.name == name)?;
        Some((
            OUTER_MARGIN + CONTENT_W / 2.0,
            card_y(i) + PROFILE_CARD_H / 2.0,
        ))
    }

    /// Writes what the open popup shows, drawn again, to a PNG (the tests' `snapshot`).
    pub(crate) fn snapshot(
        &self,
        path: &str,
        eq: &EqState,
        text: &mut TextCache,
    ) -> Result<(), String> {
        let host = self.host.as_ref().ok_or("no open menu to snapshot")?;
        let (width, height) = host.menu().menu_size().ok_or("not a menu")?;
        let hovered = self.hovered;
        let image = render_image(width, height, |cr| draw(cr, text, eq, hovered))
            .map_err(|e| e.to_string())?;
        let mut file = File::create(path).map_err(|e| e.to_string())?;
        image.write_to_png(&mut file).map_err(|e| e.to_string())
    }
}

/// Profile card `i`'s top.
fn card_y(i: usize) -> f64 {
    // At most five cards: the conversion can't fail.
    let i = f64::from(u32::try_from(i).unwrap_or(0));
    PROFILES_Y + i * (PROFILE_CARD_H + CARD_GAP)
}

/// The profile card under (x, y), if any (not the gaps between them).
fn card_at(x: f64, y: f64) -> Option<usize> {
    if !(OUTER_MARGIN..OUTER_MARGIN + CONTENT_W).contains(&x) {
        return None;
    }
    (0..PROFILES.len()).find(|&i| (card_y(i)..card_y(i) + PROFILE_CARD_H).contains(&y))
}

/// The whole popup.
fn draw(
    cr: &cairo::Context,
    text: &mut TextCache,
    eq: &EqState,
    hovered: Option<usize>,
) -> Result<(), cairo::Error> {
    popup_chrome::draw_frame(cr, POPUP_W, POPUP_H, COLORS.lavender)?;
    draw_header(cr, text, eq)?;
    draw_graph_card(cr, text, eq)?;
    text.draw_left(
        cr,
        (OUTER_MARGIN, SECTION_LABEL_Y, SECTION_LABEL_H),
        "PROFILES",
        FONT.xs,
        true,
        COLORS.overlay0,
    )?;
    for i in 0..PROFILES.len() {
        draw_profile_card(cr, text, eq, i, hovered == Some(i))?;
    }
    Ok(())
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

/// The header row. A long preset name is shortened short of the pill.
fn draw_header(
    cr: &cairo::Context,
    text: &mut TextCache,
    eq: &EqState,
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
    popup_chrome::draw_header(cr, text, (OUTER_MARGIN, OUTER_MARGIN, CONTENT_W), &header)
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

/// The graph card: a label row (preamp, peak/min), the dB grid every
/// [`GRAPH_STEP_DB`] with labels on the left, the decade frequency lines with
/// labels below, and the curve.
fn draw_graph_card(
    cr: &cairo::Context,
    text: &mut TextCache,
    eq: &EqState,
) -> Result<(), cairo::Error> {
    let snap = &eq.snap;
    let range = curve::range_db(snap, GRAPH_MIN_RANGE_DB, GRAPH_STEP_DB);
    let mid_y = GRAPH_Y + GRAPH_H / 2.0;
    let px_per_db = (GRAPH_H / 2.0) / range;

    let card = BoxRect {
        x: OUTER_MARGIN,
        y: GRAPH_CARD_Y,
        w: CONTENT_W,
        h: GRAPH_CARD_H,
    };
    popup_chrome::draw_card(cr, card, false, false, COLORS.surface1)?;

    let label_y = GRAPH_CARD_Y + CARD_PAD;
    if snap.available {
        let preamp = format!("preamp {:+.1} dB", snap.preamp);
        let extent = format!("peak {:+.1}  min {:+.1} dB", snap.peak_db, snap.min_db);
        text.draw_left(
            cr,
            (INNER_X, label_y, GRAPH_LABEL_H),
            &preamp,
            FONT.sm,
            false,
            COLORS.subtext0,
        )?;
        text.draw_right(
            cr,
            (INNER_X + INNER_W, label_y, GRAPH_LABEL_H),
            &extent,
            FONT.xs,
            false,
            COLORS.overlay0,
        )?;
    } else {
        text.draw_left(
            cr,
            (INNER_X, label_y, GRAPH_LABEL_H),
            "run blackpearl-eq.py state",
            FONT.sm,
            false,
            COLORS.subtext0,
        )?;
    }

    // The dB grid: a line per step (none at 0 dB: the curve draws a brighter
    // one), each labelled on the left.
    let mut db = -range;
    while db <= range + 0.001 {
        // +0.5: 1 px lines land on a pixel row (see curve.rs's zero line).
        let y = (mid_y - db * px_per_db).floor() + 0.5;
        let zero = db.abs() < 0.001;
        if !zero {
            stroke_line(cr, (GRAPH_X, y), (GRAPH_X + GRAPH_W, y), COLORS.surface0)?;
        }
        let label = if zero {
            "0".to_owned()
        } else {
            format!("{db:+.0}")
        };
        text.draw_right(
            cr,
            (GRAPH_X - 4.0, y - AXIS_LABEL_H / 2.0, AXIS_LABEL_H),
            &label,
            FONT.xs,
            false,
            COLORS.overlay0,
        )?;
        db += GRAPH_STEP_DB;
    }

    for (freq, label) in DECADES {
        let x = curve::freq_to_x(freq, GRAPH_X, GRAPH_W).floor() + 0.5;
        stroke_line(cr, (x, GRAPH_Y), (x, GRAPH_Y + GRAPH_H), COLORS.surface0)?;
        let label_w = text.width(label, FONT.xs, false);
        text.draw_left(
            cr,
            (
                x - label_w / 2.0,
                GRAPH_Y + GRAPH_H + FREQ_LABEL_GAP,
                AXIS_LABEL_H,
            ),
            label,
            FONT.xs,
            false,
            COLORS.overlay0,
        )?;
    }

    let accent = if snap.enabled {
        COLORS.lavender
    } else {
        COLORS.overlay0
    };
    let graph = BoxRect {
        x: GRAPH_X,
        y: GRAPH_Y,
        w: GRAPH_W,
        h: GRAPH_H,
    };
    curve::draw(cr, snap, graph, range, (accent, 2.0), Some(COLORS.surface2))
}

/// Profile card `i`, in the audio device cards' language: the active one has
/// the accent border and name, a hovered one lightens to show it's clickable,
/// and a small tag says what a click does or is doing. While a switch is
/// under way nothing is clickable, so nothing lights up or offers `apply`.
fn draw_profile_card(
    cr: &cairo::Context,
    text: &mut TextCache,
    eq: &EqState,
    i: usize,
    hovered: bool,
) -> Result<(), cairo::Error> {
    let Some(profile) = PROFILES.get(i) else {
        return Ok(());
    };
    let active = eq.snap.profile == profile.name;
    let pending = eq.pending.as_deref() == Some(profile.name);
    let hovered = hovered && !eq.busy();
    let y = card_y(i);

    let card = BoxRect {
        x: OUTER_MARGIN,
        y,
        w: CONTENT_W,
        h: PROFILE_CARD_H,
    };
    popup_chrome::draw_card(cr, card, active, hovered, COLORS.lavender)?;

    let name_color = if active { COLORS.lavender } else { COLORS.text };
    text.draw_left(
        cr,
        (INNER_X, y, PROFILE_CARD_H),
        profile.name,
        FONT.label,
        true,
        name_color,
    )?;

    let tag = if pending {
        Some(("applying...", COLORS.yellow))
    } else if active {
        Some(("active", COLORS.lavender))
    } else if hovered {
        Some(("apply", COLORS.overlay0))
    } else {
        None
    };
    if let Some((tag, color)) = tag {
        let tag_x = INNER_X + text.width(profile.name, FONT.label, true) + COL_GAP;
        text.draw_left(cr, (tag_x, y, PROFILE_CARD_H), tag, FONT.xs, true, color)?;
    }

    text.draw_right(
        cr,
        (INNER_X + INNER_W, y, PROFILE_CARD_H),
        profile.description,
        FONT.sm,
        false,
        COLORS.subtext0,
    )
}
