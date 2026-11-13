//! The device dropdown, one for both sides: right click on the volume box
//! lists the sinks, on the mic box the sources (the C bar's
//! `audio/device_popup.c`; `docs/modules/audio.md` describes it).
//!
//! Header `Output|Input · N devices`, with a pill showing the default's
//! volume or "muted". Then one card per device:
//! - name (accent plus a `default` tag on the default, whose card also gets
//!   the accent border; a `set default` tag on a hovered other card);
//! - a detail line, `USB · 48 kHz · 2 ch · running`, the state coloured;
//! - a 30-segment volume meter (0-100 %, full with a peach end above that),
//!   the percent, and a mute chip.
//!
//! Input: press on the meter sets the volume and starts a drag (it follows
//! the pointer until the release, wherever that lands); scroll on a card
//! steps it ±5 %; the mute chip mutes; a click anywhere else on a card makes
//! that device the default. Monitor sources (a sink's `.monitor`) are left
//! out of the mic list, unless one is the default.
//!
//! The list follows the server live (`refresh`): when the number of cards
//! changes, the popup is resized (`PopupHost::resize`).

use std::fs::File;

use smithay_client_toolkit::reexports::client::protocol::wl_output::WlOutput;

use super::{AudioState, Command, Kind, Node, VOLUME_STEP, volume_style};
use crate::{
    bar::{
        popup_host::PopupHost,
        popup_surface::{PopupEnv, PopupSurface, render_image},
    },
    config::{
        COLORS, Color, FONT,
        glyphs::{ICON_HEADPHONES, ICON_MIC, ICON_MIC_MUTED},
    },
    render::{
        BoxRect, meter_bar,
        module_box::draw_rect,
        popup_chrome::{self, Header},
        popup_style::{CARD_GAP, COL_GAP, HEADER_H, OUTER_MARGIN, SECTION_GAP},
        text::TextCache,
        whole_px,
    },
};

/// The content column's width.
const CONTENT_W: f64 = 340.0;
/// Card padding, all sides.
const CARD_PAD: f64 = 10.0;
/// The name line's height.
const NAME_H: f64 = 18.0;
/// The detail line's height.
const DETAIL_H: f64 = 14.0;
/// Between the detail line and the controls.
const CONTROL_GAP: f64 = 6.0;
/// The mute chip's size.
const CHIP_W: f64 = 30.0;
/// The mute chip's (and the controls row's) height.
const CHIP_H: f64 = 22.0;
/// The meter's thickness.
const METER_H: f64 = 6.0;
/// The meter's segments.
const METER_SEGMENTS: u32 = 30;
/// Extra grab room past both meter ends, so 0 % and 100 % are easy to hit.
const METER_SLOP: f64 = 4.0;
/// The meter spans 0-100 %: above 100 is rarely wanted, so the width goes to
/// the useful range (scrolling still goes to 200).
const METER_MAX: u32 = 100;
/// A card's height.
const CARD_H: f64 = CARD_PAD + NAME_H + DETAIL_H + CONTROL_GAP + CHIP_H + CARD_PAD;
/// Where card content starts.
const INNER_X: f64 = OUTER_MARGIN + CARD_PAD;
/// Card content's width.
const INNER_W: f64 = CONTENT_W - CARD_PAD * 2.0;
/// The mute chip's left edge.
const MUTE_X: f64 = INNER_X + INNER_W - CHIP_W;

/// Which part of a card the pointer is on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Part {
    /// Anywhere but the controls: a click makes the device the default.
    Body,
    /// The meter (with some slop): a press sets the volume.
    Meter,
    /// The mute chip.
    Mute,
}

/// One card: the device's server index and the card's top.
#[derive(Clone, Copy, Debug)]
struct Card {
    /// The device's index (cards look their device up on every paint, so
    /// they always show the latest figures).
    index: u32,
    /// The card's top, popup-local.
    y: f64,
}

/// A meter drag in progress.
#[derive(Clone, Copy, Debug)]
struct Drag {
    /// The device being dragged.
    index: u32,
    /// The percent last sent: drawn instead of the server's figure, so the
    /// meter follows the pointer without waiting for the round trip, and
    /// not sent again while the pointer stays on it.
    percent: u32,
}

/// The open dropdown's state; `None` when closed.
struct Open {
    /// The surfaces.
    host: PopupHost,
    /// Sinks (volume box) or sources (mic box).
    kind: Kind,
    /// The opening box's colour.
    accent: Color,
    /// The cards, top to bottom.
    cards: Vec<Card>,
    /// The card and part under the pointer.
    hovered: Option<(usize, Part)>,
    /// A meter drag, from press to release.
    drag: Option<Drag>,
    /// The popup's size.
    size: (f64, f64),
}

/// The device dropdown: closed or open.
#[derive(Default)]
pub(crate) struct DevicePopup {
    /// The open state.
    open: Option<Open>,
}

impl DevicePopup {
    /// Opens the `kind` list under the box `trigger` on `output`, re-opening
    /// it if it's open. Nothing opens if there are no devices at all.
    pub(crate) fn open<'o>(
        &mut self,
        env: &PopupEnv<'_>,
        (output, output_width): (&WlOutput, i32),
        (trigger, kind, accent): (BoxRect, Kind, Color),
        all_outputs: impl Iterator<Item = &'o WlOutput>,
        (audio, text): (&AudioState, &mut TextCache),
    ) {
        self.open = None;
        let (cards, size) = layout(audio, kind);
        if cards.is_empty() {
            return;
        }
        let host =
            PopupHost::open_right(env, (output, output_width), trigger, px(size), all_outputs);
        self.open = Some(Open {
            host,
            kind,
            accent,
            cards,
            hovered: None,
            drag: None,
            size,
        });
        self.paint(audio, text);
    }

    /// A new audio snapshot: re-lay out the cards, resize if their number
    /// changed, repaint. Closes if no device is left.
    pub(crate) fn refresh(&mut self, env: &PopupEnv<'_>, audio: &AudioState, text: &mut TextCache) {
        let Some(open) = self.open.as_mut() else {
            return;
        };
        let (cards, size) = layout(audio, open.kind);
        if cards.is_empty() {
            self.open = None;
            return;
        }
        if open.hovered.is_some_and(|(card, _)| card >= cards.len()) {
            open.hovered = None;
        }
        open.cards = cards;
        if size != open.size {
            open.size = size;
            // A drag in progress loses its grab with the old surface; the
            // next release still ends it.
            open.host.resize(env, px(size));
        }
        self.paint(audio, text);
    }

    /// Closes the dropdown (no-op if closed).
    pub(crate) fn close(&mut self) {
        self.open = None;
    }

    /// The open popup's surfaces, if open.
    pub(crate) fn host(&self) -> Option<&PopupHost> {
        self.open.as_ref().map(|o| &o.host)
    }

    /// The open popup's surfaces, for configure routing.
    pub(crate) fn host_mut(&mut self) -> Option<&mut PopupHost> {
        self.open.as_mut().map(|o| &mut o.host)
    }

    /// The menu surface, if open.
    pub(crate) fn menu(&self) -> Option<&PopupSurface> {
        self.host().map(PopupHost::menu)
    }

    /// Whether (x, y) on the menu is on a card (all of a card reacts to a
    /// click: the pointer turns into a hand over it).
    pub(crate) fn over_card(&self, x: f64, y: f64) -> bool {
        self.open
            .as_ref()
            .is_some_and(|o| card_at(&o.cards, (x, y), 0.0).is_some())
    }

    /// How many cards are shown (the tests' `menu`), `None` when closed.
    pub(crate) fn card_count(&self) -> Option<usize> {
        self.open.as_ref().map(|o| o.cards.len())
    }

    /// Paints the open popup.
    pub(crate) fn paint(&mut self, audio: &AudioState, text: &mut TextCache) {
        if let Some(open) = self.open.as_mut() {
            let view = View::of(open);
            open.host
                .menu_mut()
                .paint(|cr| draw(cr, text, audio, &view));
        }
    }

    /// The pointer moved to (x, y) on the menu: a drag follows it (returning
    /// the volume to send), otherwise the hover highlight does.
    pub(crate) fn motion(
        &mut self,
        (x, y): (f64, f64),
        audio: &AudioState,
        text: &mut TextCache,
    ) -> Option<Command> {
        let open = self.open.as_mut()?;
        if let Some(drag) = open.drag {
            return self.drag_to(x, drag, audio, text);
        }
        let hovered = card_at(&open.cards, (x, y), meter_w(text));
        if hovered != open.hovered {
            open.hovered = hovered;
            self.paint(audio, text);
        }
        None
    }

    /// A button press at (x, y) on the menu. Returns what to send.
    pub(crate) fn press(
        &mut self,
        (x, y): (f64, f64),
        audio: &AudioState,
        text: &mut TextCache,
    ) -> Option<Command> {
        let open = self.open.as_mut()?;
        let (card, part) = card_at(&open.cards, (x, y), meter_w(text))?;
        let index = open.cards.get(card)?.index;
        let kind = open.kind;
        match part {
            Part::Mute => Some(Command::ToggleMute { kind, index }),
            Part::Meter => {
                // `u32::MAX` never matches a real percent, so the first
                // position is always sent.
                let drag = Drag {
                    index,
                    percent: u32::MAX,
                };
                open.drag = Some(drag);
                self.drag_to(x, drag, audio, text)
            }
            // The rest of the card makes it the default (the reference bar's
            // click on the device name). The popup stays open.
            Part::Body => audio.node(kind, index).map(|node| Command::SetDefault {
                kind,
                name: node.name.clone(),
            }),
        }
    }

    /// A button was released anywhere: ends a drag (the meter then shows the
    /// server's figure again).
    pub(crate) fn release(&mut self, audio: &AudioState, text: &mut TextCache) {
        if let Some(open) = self.open.as_mut()
            && open.drag.take().is_some()
        {
            self.paint(audio, text);
        }
    }

    /// One scroll step at (x, y) on the menu: ±5 % on the card under it
    /// (not during a drag).
    pub(crate) fn scroll(
        &self,
        (x, y): (f64, f64),
        up: bool,
        text: &mut TextCache,
    ) -> Option<Command> {
        let open = self.open.as_ref().filter(|o| o.drag.is_none())?;
        let (card, _) = card_at(&open.cards, (x, y), meter_w(text))?;
        Some(Command::AdjustVolume {
            kind: open.kind,
            index: open.cards.get(card)?.index,
            delta: if up { VOLUME_STEP } else { -VOLUME_STEP },
        })
    }

    /// Moves a drag to `x`: the volume to send, if it changed.
    fn drag_to(
        &mut self,
        x: f64,
        drag: Drag,
        audio: &AudioState,
        text: &mut TextCache,
    ) -> Option<Command> {
        let open = self.open.as_mut()?;
        let percent = volume_style::slider_percent(x, (INNER_X, meter_w(text)), METER_MAX);
        if percent == drag.percent {
            return None; // don't flood the server with the same value every pixel
        }
        open.drag = Some(Drag { percent, ..drag });
        let kind = open.kind;
        self.paint(audio, text);
        Some(Command::SetVolume {
            kind,
            index: drag.index,
            percent,
        })
    }

    /// Where a control of the card for `description` is, menu-local (the
    /// tests' `target device <part> <frac> <description>`): `meter` at
    /// `fraction` along it, `mute`, or `body` (the name line).
    pub(crate) fn target(
        &self,
        (description, part, fraction): (&str, &str, f64),
        audio: &AudioState,
        text: &mut TextCache,
    ) -> Option<(f64, f64)> {
        let open = self.open.as_ref()?;
        let card = open.cards.iter().find(|card| {
            audio
                .node(open.kind, card.index)
                .is_some_and(|n| n.description == description)
        })?;
        let controls_y = card.y + CARD_PAD + NAME_H + DETAIL_H + CONTROL_GAP + CHIP_H / 2.0;
        Some(match part {
            "meter" => (INNER_X + fraction * meter_w(text), controls_y),
            "mute" => (MUTE_X + CHIP_W / 2.0, controls_y),
            _ => (INNER_X + 20.0, card.y + CARD_PAD + NAME_H / 2.0),
        })
    }

    /// Writes what the open popup shows, drawn again, to a PNG (the tests' `snapshot`).
    pub(crate) fn snapshot(
        &self,
        path: &str,
        audio: &AudioState,
        text: &mut TextCache,
    ) -> Result<(), String> {
        let open = self.open.as_ref().ok_or("no open menu to snapshot")?;
        let (width, height) = open.host.menu().menu_size().ok_or("not a menu")?;
        let view = View::of(open);
        let image = render_image(width, height, |cr| draw(cr, text, audio, &view))
            .map_err(|e| e.to_string())?;
        let mut file = File::create(path).map_err(|e| e.to_string())?;
        image.write_to_png(&mut file).map_err(|e| e.to_string())
    }
}

/// What painting needs from the open state (a copy, so the surface can be
/// borrowed mutably while drawing).
struct View {
    /// Sinks or sources.
    kind: Kind,
    /// The accent colour.
    accent: Color,
    /// The cards.
    cards: Vec<Card>,
    /// What's hovered.
    hovered: Option<(usize, Part)>,
    /// The drag, if any.
    drag: Option<Drag>,
    /// The popup's size.
    size: (f64, f64),
}

impl View {
    /// The view of `open`.
    fn of(open: &Open) -> Self {
        Self {
            kind: open.kind,
            accent: open.accent,
            cards: open.cards.clone(),
            hovered: open.hovered,
            drag: open.drag,
            size: open.size,
        }
    }
}

/// The cards to show for `kind`, and the popup size they make.
fn layout(audio: &AudioState, kind: Kind) -> (Vec<Card>, (f64, f64)) {
    let mut y = OUTER_MARGIN + HEADER_H + SECTION_GAP;
    let mut cards = Vec::new();
    for node in audio.nodes(kind) {
        // Monitors aren't microphones; one stays visible if it's the default,
        // so it can be seen and switched away from.
        if node.monitor && !audio.is_default(kind, node) {
            continue;
        }
        if !cards.is_empty() {
            y += CARD_GAP;
        }
        cards.push(Card {
            index: node.index,
            y,
        });
        y += CARD_H;
    }
    (cards, (CONTENT_W + OUTER_MARGIN * 2.0, y + OUTER_MARGIN))
}

/// A size in whole px, for Wayland.
fn px((w, h): (f64, f64)) -> (i32, i32) {
    (whole_px(w), whole_px(h))
}

/// The meter's width: what's left of the row after the percent and the chip.
fn meter_w(text: &mut TextCache) -> f64 {
    MUTE_X - COL_GAP - text.width("200%", FONT.sm, false) - COL_GAP - INNER_X
}

/// The card and part at (x, y), if any, with the meter `meter_w` wide.
fn card_at(cards: &[Card], (x, y): (f64, f64), meter_w: f64) -> Option<(usize, Part)> {
    if !(OUTER_MARGIN..OUTER_MARGIN + CONTENT_W).contains(&x) {
        return None;
    }
    let found = cards
        .iter()
        .position(|c| (c.y..c.y + CARD_H).contains(&y))?;
    let card = cards.get(found)?;
    let controls_y = card.y + CARD_PAD + NAME_H + DETAIL_H + CONTROL_GAP;
    let in_controls = (controls_y..controls_y + CHIP_H).contains(&y);
    // The percent column between meter and chip counts as the body, as in C.
    let part = if in_controls && (MUTE_X..MUTE_X + CHIP_W).contains(&x) {
        Part::Mute
    } else if in_controls && (INNER_X - METER_SLOP..INNER_X + meter_w + METER_SLOP).contains(&x) {
        Part::Meter
    } else {
        Part::Body
    };
    Some((found, part))
}

/// The whole popup.
fn draw(
    cr: &cairo::Context,
    text: &mut TextCache,
    audio: &AudioState,
    view: &View,
) -> Result<(), cairo::Error> {
    popup_chrome::draw_frame(cr, view.size.0, view.size.1, view.accent)?;
    draw_header(cr, text, audio, view)?;
    for (i, card) in view.cards.iter().enumerate() {
        if let Some(node) = audio.node(view.kind, card.index) {
            draw_card(cr, text, (audio, node, card), view, i)?;
        }
    }
    Ok(())
}

/// `Output · 3 devices`, and the default's volume (or "muted") in a pill.
fn draw_header(
    cr: &cairo::Context,
    text: &mut TextCache,
    audio: &AudioState,
    view: &View,
) -> Result<(), cairo::Error> {
    let count = view.cards.len();
    let summary = format!(
        "  \u{b7}  {count} device{}",
        if count == 1 { "" } else { "s" }
    );
    let (icon, title) = match view.kind {
        Kind::Sink => (ICON_HEADPHONES, "Output"),
        Kind::Source => (ICON_MIC, "Input"),
    };
    // Only if the default is among the cards (the server may not have named
    // one yet).
    let default = view
        .cards
        .iter()
        .filter_map(|c| audio.node(view.kind, c.index))
        .find(|n| audio.is_default(view.kind, n));
    let word = default.map(|n| {
        if n.muted {
            "muted".to_owned()
        } else {
            format!("{}%", shown_percent(n, view))
        }
    });
    let header = Header {
        icon: Some(icon),
        icon_color: view.accent,
        title,
        summary: &summary,
        pill: word.as_deref(),
        pill_fill: if default.is_some_and(|n| n.muted) {
            COLORS.red
        } else {
            view.accent
        },
    };
    popup_chrome::draw_header(cr, text, (OUTER_MARGIN, OUTER_MARGIN, CONTENT_W), &header)
}

/// The percent to draw for `node`: the drag's while it's being dragged.
fn shown_percent(node: &Node, view: &View) -> u32 {
    view.drag
        .filter(|d| d.index == node.index)
        .map_or(node.volume, |d| d.percent)
}

/// `USB · 48 kHz · 2 ch`, leaving out what the device doesn't report (the
/// state word is drawn after it, in its own colour).
fn detail_text(node: &Node) -> String {
    let mut parts = Vec::new();
    if !node.bus.is_empty() {
        parts.push(node.bus.clone());
    }
    if node.rate > 0 {
        parts.push(if node.rate.is_multiple_of(1000) {
            format!("{} kHz", node.rate / 1000)
        } else {
            format!("{:.1} kHz", f64::from(node.rate) / 1000.0)
        });
    }
    if node.channels > 0 {
        parts.push(format!("{} ch", node.channels));
    }
    parts.join(" \u{b7} ")
}

/// The mute chip's icon: the sink side uses the volume box's four levels,
/// the source side the mic's two (the reference bar's two different
/// `muteIcon` functions).
const fn status_glyph(kind: Kind, percent: u32, muted: bool) -> char {
    match kind {
        Kind::Sink => volume_style::level_glyph(percent, muted),
        Kind::Source if muted => ICON_MIC_MUTED,
        Kind::Source => ICON_MIC,
    }
}

/// One device card.
fn draw_card(
    cr: &cairo::Context,
    text: &mut TextCache,
    (audio, node, card): (&AudioState, &Node, &Card),
    view: &View,
    position: usize,
) -> Result<(), cairo::Error> {
    let is_default = audio.is_default(view.kind, node);
    let hovered = |part| view.hovered == Some((position, part));
    let body_hovered = hovered(Part::Body);
    let rect = BoxRect {
        x: OUTER_MARGIN,
        y: card.y,
        w: CONTENT_W,
        h: CARD_H,
    };
    // The default gets the accent border; a hovered other card lightens to
    // show it can be clicked.
    popup_chrome::draw_card(
        cr,
        rect,
        is_default,
        body_hovered && !is_default,
        view.accent,
    )?;

    // Name line, with the tag on the right.
    let mut y = card.y + CARD_PAD;
    let tag = if is_default {
        "default"
    } else if body_hovered {
        "set default"
    } else {
        ""
    };
    let tag_w = if tag.is_empty() {
        0.0
    } else {
        text.width(tag, FONT.xs, true) + COL_GAP
    };
    let name = text.truncate(&node.description, INNER_W - tag_w, FONT.label, true);
    let name_color = if is_default { view.accent } else { COLORS.text };
    text.draw_left(
        cr,
        (INNER_X, y, NAME_H),
        &name,
        FONT.label,
        true,
        name_color,
    )?;
    if !tag.is_empty() {
        let tag_color = if is_default {
            view.accent
        } else {
            COLORS.overlay0
        };
        text.draw_right(
            cr,
            (INNER_X + INNER_W, y, NAME_H),
            tag,
            FONT.xs,
            true,
            tag_color,
        )?;
    }
    y += NAME_H;

    // Detail line, then the state word in its colour.
    let detail = detail_text(node);
    text.draw_left(
        cr,
        (INNER_X, y, DETAIL_H),
        &detail,
        FONT.xs,
        false,
        COLORS.overlay0,
    )?;
    if !node.state.is_empty() {
        let color = match node.state {
            "running" => COLORS.green,
            "idle" => COLORS.yellow,
            _ => COLORS.overlay0,
        };
        let separator = if detail.is_empty() { "" } else { " \u{b7} " };
        let tail = format!("{separator}{}", node.state);
        let x = INNER_X + text.width(&detail, FONT.xs, false);
        text.draw_left(cr, (x, y, DETAIL_H), &tail, FONT.xs, false, color)?;
    }

    draw_controls(cr, text, (node, card), view, hovered(Part::Mute))
}

/// A card's controls row: the volume meter, the percent and the mute chip.
fn draw_controls(
    cr: &cairo::Context,
    text: &mut TextCache,
    (node, card): (&Node, &Card),
    view: &View,
    mute_hovered: bool,
) -> Result<(), cairo::Error> {
    let percent = shown_percent(node, view);
    let controls_y = card.y + CARD_PAD + NAME_H + DETAIL_H + CONTROL_GAP;
    let meter = BoxRect {
        x: INNER_X,
        y: controls_y + (CHIP_H - METER_H) / 2.0,
        w: meter_w(text),
        h: METER_H,
    };
    let gradient = volume_style::meter_gradient(view.accent, node.muted, percent);
    let fraction = f64::from(percent) / f64::from(METER_MAX);
    meter_bar::draw(
        cr,
        meter,
        fraction,
        METER_SEGMENTS,
        &gradient,
        COLORS.surface0,
    )?;
    let percent_color = if node.muted {
        COLORS.overlay0
    } else {
        COLORS.subtext0
    };
    let label = format!("{percent}%");
    text.draw_right(
        cr,
        (MUTE_X - COL_GAP, controls_y, CHIP_H),
        &label,
        FONT.sm,
        false,
        percent_color,
    )?;
    let chip = BoxRect {
        x: MUTE_X,
        y: controls_y,
        w: CHIP_W,
        h: CHIP_H,
    };
    let chip_fill = if mute_hovered {
        COLORS.surface1
    } else {
        COLORS.surface0
    };
    draw_rect(cr, chip, chip_fill, COLORS.mantle, 0.0)?;
    let glyph = status_glyph(view.kind, percent, node.muted).to_string();
    let glyph_color = if node.muted {
        COLORS.overlay0
    } else {
        view.accent
    };
    text.draw_centered(cr, chip, &glyph, FONT.heading, false, glyph_color)
}
