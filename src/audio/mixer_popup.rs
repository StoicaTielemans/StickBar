//! The per-app mixer: middle click on the volume box (the C bar's
//! `audio/mixer_popup.c`; no reference-bar equivalent, added at the user's
//! request).
//!
//! Header `Mixer · N apps · <default sink>`, with the default sink's volume
//! (or "muted") in a pill. Then one card per app playback stream: the name,
//! the percent and a mute chip on top, a full-width 36-segment slider
//! (0-100 %) under them. With nothing playing, one card says so: the popup
//! still opens, so the middle click visibly did something.
//!
//! Input: press on a slider sets the volume and starts a drag; scroll on a
//! card steps ±5 % (up to 200 %, past the slider's end: the slider then shows
//! full with a peach end); the chip mutes. Apps start and stop streams far
//! more often than devices come and go, so this popup resizes often
//! (`PopupHost::resize`).

use std::fs::File;

use smithay_client_toolkit::reexports::client::protocol::wl_output::WlOutput;

use super::{AudioState, Command, Kind, Stream, VOLUME_STEP, volume_style};
use crate::{
    bar::{
        popup_host::PopupHost,
        popup_surface::{PopupEnv, PopupSurface, render_image},
    },
    config::{COLORS, Color, FONT, glyphs::ICON_VOLUME_HIGH},
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
/// The mute chip's width.
const CHIP_W: f64 = 30.0;
/// The mute chip's (and the top row's) height.
const CHIP_H: f64 = 22.0;
/// Between the top row and the slider.
const SLIDER_GAP: f64 = 6.0;
/// The slider row's height (its hit area; the bar itself is `METER_H`).
const SLIDER_ROW_H: f64 = 12.0;
/// The slider's thickness.
const METER_H: f64 = 6.0;
/// The slider's segments.
const METER_SEGMENTS: u32 = 36;
/// Extra grab room past both slider ends.
const METER_SLOP: f64 = 4.0;
/// The empty-state card's height.
const EMPTY_CARD_H: f64 = 40.0;
/// The slider spans 0-100 %: per-app boosting past 100 is rarely wanted.
const SLIDER_MAX: u32 = 100;
/// The empty state's text.
const EMPTY_TEXT: &str = "No applications playing audio";
/// A card's height.
const CARD_H: f64 = CARD_PAD + CHIP_H + SLIDER_GAP + SLIDER_ROW_H + CARD_PAD;
/// Where card content starts (and the slider).
const INNER_X: f64 = OUTER_MARGIN + CARD_PAD;
/// Card content's width (and the slider's).
const INNER_W: f64 = CONTENT_W - CARD_PAD * 2.0;
/// The mute chip's left edge.
const MUTE_X: f64 = INNER_X + INNER_W - CHIP_W;

/// One card: the stream's server index and the card's top.
#[derive(Clone, Copy, Debug)]
struct Row {
    /// The stream's index (looked up on every paint).
    index: u32,
    /// The card's top, popup-local.
    y: f64,
}

/// A slider drag in progress.
#[derive(Clone, Copy, Debug)]
struct Drag {
    /// The stream being dragged.
    index: u32,
    /// The percent last sent (drawn instead of the server's figure).
    percent: u32,
}

/// The open mixer's state.
struct Open {
    /// The surfaces.
    host: PopupHost,
    /// The volume box's colour.
    accent: Color,
    /// The cards, top to bottom.
    rows: Vec<Row>,
    /// The hovered card, and whether the pointer is on its mute chip.
    hovered: Option<(usize, bool)>,
    /// A slider drag, from press to release.
    drag: Option<Drag>,
    /// The popup's size.
    size: (f64, f64),
}

/// The mixer: closed or open.
#[derive(Default)]
pub(crate) struct MixerPopup {
    /// The open state.
    open: Option<Open>,
}

impl MixerPopup {
    /// Opens the mixer under the box `trigger` on `output`, re-opening it if
    /// open. It opens even with no streams (the empty-state card).
    pub(crate) fn open<'o>(
        &mut self,
        env: &PopupEnv<'_>,
        (output, output_width): (&WlOutput, i32),
        (trigger, accent): (BoxRect, Color),
        all_outputs: impl Iterator<Item = &'o WlOutput>,
        (audio, text): (&AudioState, &mut TextCache),
    ) {
        self.open = None;
        let (rows, size) = layout(audio);
        let host =
            PopupHost::open_right(env, (output, output_width), trigger, px(size), all_outputs);
        self.open = Some(Open {
            host,
            accent,
            rows,
            hovered: None,
            drag: None,
            size,
        });
        self.paint(audio, text);
    }

    /// A new audio snapshot: re-lay out, resize if the row count changed,
    /// repaint.
    pub(crate) fn refresh(&mut self, env: &PopupEnv<'_>, audio: &AudioState, text: &mut TextCache) {
        let Some(open) = self.open.as_mut() else {
            return;
        };
        let (rows, size) = layout(audio);
        if open.hovered.is_some_and(|(row, _)| row >= rows.len()) {
            open.hovered = None;
        }
        open.rows = rows;
        if size != open.size {
            // An app started or stopped playing. A drag in progress loses its
            // grab with the old surface; the next release still ends it.
            open.size = size;
            open.host.resize(env, px(size));
        }
        self.paint(audio, text);
    }

    /// Closes the mixer (no-op if closed).
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

    /// How many app rows are shown (the tests' `menu`), `None` when closed.
    pub(crate) fn row_count(&self) -> Option<usize> {
        self.open.as_ref().map(|o| o.rows.len())
    }

    /// Whether (x, y) on the menu is on a mute chip or a slider (the hand cursor).
    pub(crate) fn over_control(&self, x: f64, y: f64) -> bool {
        self.open.as_ref().is_some_and(|o| {
            row_at(&o.rows, x, y)
                .and_then(|i| o.rows.get(i))
                .is_some_and(|row| on_mute_chip(row, x, y) || on_slider(row, x, y))
        })
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
        let hovered =
            row_at(&open.rows, x, y).and_then(|i| Some((i, on_mute_chip(open.rows.get(i)?, x, y))));
        if hovered != open.hovered {
            open.hovered = hovered;
            self.paint(audio, text);
        }
        None
    }

    /// A left press at (x, y) on the menu: the mute chip, or the slider
    /// (starts a drag). The name and percent do nothing.
    pub(crate) fn press(
        &mut self,
        (x, y): (f64, f64),
        audio: &AudioState,
        text: &mut TextCache,
    ) -> Option<Command> {
        let open = self.open.as_mut()?;
        let row = *open.rows.get(row_at(&open.rows, x, y)?)?;
        if on_mute_chip(&row, x, y) {
            return Some(Command::ToggleStreamMute { index: row.index });
        }
        if !on_slider(&row, x, y) {
            return None;
        }
        // `u32::MAX` never matches a real percent, so the first is always sent.
        let drag = Drag {
            index: row.index,
            percent: u32::MAX,
        };
        open.drag = Some(drag);
        self.drag_to(x, drag, audio, text)
    }

    /// A button was released anywhere: ends a drag.
    pub(crate) fn release(&mut self, audio: &AudioState, text: &mut TextCache) {
        if let Some(open) = self.open.as_mut()
            && open.drag.take().is_some()
        {
            self.paint(audio, text);
        }
    }

    /// One scroll step at (x, y) on the menu: ±5 % on the card under it.
    pub(crate) fn scroll(&self, (x, y): (f64, f64), up: bool) -> Option<Command> {
        let open = self.open.as_ref().filter(|o| o.drag.is_none())?;
        let row = open.rows.get(row_at(&open.rows, x, y)?)?;
        Some(Command::AdjustStreamVolume {
            index: row.index,
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
        let percent = volume_style::slider_percent(x, (INNER_X, INNER_W), SLIDER_MAX);
        if percent == drag.percent {
            return None; // don't flood the server with the same value every pixel
        }
        open.drag = Some(Drag { percent, ..drag });
        self.paint(audio, text);
        Some(Command::SetStreamVolume {
            index: drag.index,
            percent,
        })
    }

    /// Where a control of the card for stream `name` is, menu-local (the
    /// tests' `target mixer <part> <frac> <name>`): `mute`, or the slider at
    /// `fraction` along it.
    pub(crate) fn target(
        &self,
        (name, part, fraction): (&str, &str, f64),
        audio: &AudioState,
    ) -> Option<(f64, f64)> {
        let open = self.open.as_ref()?;
        let row = open
            .rows
            .iter()
            .find(|row| audio.stream(row.index).is_some_and(|s| s.name == name))?;
        Some(if part == "mute" {
            (MUTE_X + CHIP_W / 2.0, row.y + CARD_PAD + CHIP_H / 2.0)
        } else {
            (
                INNER_X + fraction * INNER_W,
                slider_y(row) + SLIDER_ROW_H / 2.0,
            )
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
    /// The accent colour.
    accent: Color,
    /// The cards.
    rows: Vec<Row>,
    /// What's hovered.
    hovered: Option<(usize, bool)>,
    /// The drag, if any.
    drag: Option<Drag>,
    /// The popup's size.
    size: (f64, f64),
}

impl View {
    /// The view of `open`.
    fn of(open: &Open) -> Self {
        Self {
            accent: open.accent,
            rows: open.rows.clone(),
            hovered: open.hovered,
            drag: open.drag,
            size: open.size,
        }
    }
}

/// The rows for every stream, and the popup size they make (one
/// empty-state card when there are none).
fn layout(audio: &AudioState) -> (Vec<Row>, (f64, f64)) {
    let mut y = OUTER_MARGIN + HEADER_H + SECTION_GAP;
    let mut rows = Vec::new();
    for stream in &audio.streams {
        if !rows.is_empty() {
            y += CARD_GAP;
        }
        rows.push(Row {
            index: stream.index,
            y,
        });
        y += CARD_H;
    }
    if rows.is_empty() {
        y += EMPTY_CARD_H;
    }
    (rows, (CONTENT_W + OUTER_MARGIN * 2.0, y + OUTER_MARGIN))
}

/// A size in whole px, for Wayland.
fn px((w, h): (f64, f64)) -> (i32, i32) {
    (whole_px(w), whole_px(h))
}

/// The top of `row`'s slider row.
fn slider_y(row: &Row) -> f64 {
    row.y + CARD_PAD + CHIP_H + SLIDER_GAP
}

/// The row at (x, y), if any.
fn row_at(rows: &[Row], x: f64, y: f64) -> Option<usize> {
    if !(OUTER_MARGIN..OUTER_MARGIN + CONTENT_W).contains(&x) {
        return None;
    }
    rows.iter()
        .position(|row| (row.y..row.y + CARD_H).contains(&y))
}

/// Whether (x, y) is on `row`'s mute chip.
fn on_mute_chip(row: &Row, x: f64, y: f64) -> bool {
    let top = row.y + CARD_PAD;
    (MUTE_X..MUTE_X + CHIP_W).contains(&x) && (top..top + CHIP_H).contains(&y)
}

/// Whether (x, y) is on `row`'s slider (with some slop past its ends).
fn on_slider(row: &Row, x: f64, y: f64) -> bool {
    let top = slider_y(row);
    (top..top + SLIDER_ROW_H).contains(&y)
        && (INNER_X - METER_SLOP..INNER_X + INNER_W + METER_SLOP).contains(&x)
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
    if view.rows.is_empty() {
        let rect = BoxRect {
            x: OUTER_MARGIN,
            y: OUTER_MARGIN + HEADER_H + SECTION_GAP,
            w: CONTENT_W,
            h: EMPTY_CARD_H,
        };
        popup_chrome::draw_card(cr, rect, false, false, COLORS.surface1)?;
        return text.draw_left(
            cr,
            (INNER_X, rect.y, rect.h),
            EMPTY_TEXT,
            FONT.label,
            false,
            COLORS.subtext0,
        );
    }
    for (i, row) in view.rows.iter().enumerate() {
        if let Some(stream) = audio.stream(row.index) {
            draw_row(cr, text, (stream, row), view, i)?;
        }
    }
    Ok(())
}

/// `Mixer · 3 apps · TE-C Analog Stereo`, and the default sink's volume (or
/// "muted") in a pill: the device these streams mostly play through.
fn draw_header(
    cr: &cairo::Context,
    text: &mut TextCache,
    audio: &AudioState,
    view: &View,
) -> Result<(), cairo::Error> {
    let count = view.rows.len();
    let sink = audio.default_node(Kind::Sink);
    let sink_name = sink.map_or("", |n| n.description.as_str());
    let summary = format!(
        "  \u{b7}  {count} app{}{}{sink_name}",
        if count == 1 { "" } else { "s" },
        if sink_name.is_empty() {
            ""
        } else {
            "  \u{b7}  "
        },
    );
    let (volume, muted) = sink.map_or((0, false), |n| (n.volume, n.muted));
    let word = if muted {
        "muted".to_owned()
    } else {
        format!("{volume}%")
    };
    let header = Header {
        icon: Some(ICON_VOLUME_HIGH),
        icon_color: view.accent,
        title: "Mixer",
        summary: &summary,
        pill: Some(&word),
        pill_fill: if muted { COLORS.red } else { view.accent },
    };
    popup_chrome::draw_header(cr, text, (OUTER_MARGIN, OUTER_MARGIN, CONTENT_W), &header)
}

/// One app's card.
fn draw_row(
    cr: &cairo::Context,
    text: &mut TextCache,
    (stream, row): (&Stream, &Row),
    view: &View,
    position: usize,
) -> Result<(), cairo::Error> {
    let percent = view
        .drag
        .filter(|d| d.index == stream.index)
        .map_or(stream.volume, |d| d.percent);
    let hovered = view.hovered.filter(|&(i, _)| i == position);
    let rect = BoxRect {
        x: OUTER_MARGIN,
        y: row.y,
        w: CONTENT_W,
        h: CARD_H,
    };
    popup_chrome::draw_card(cr, rect, false, hovered.is_some(), COLORS.surface1)?;

    // Top row: name, percent, mute chip.
    let y = row.y + CARD_PAD;
    let percent_w = text.width("200%", FONT.sm, false);
    let name_max_w = MUTE_X - COL_GAP - percent_w - COL_GAP - INNER_X;
    let name = text.truncate(&stream.name, name_max_w, FONT.label, true);
    let (name_color, percent_color, glyph_color) = if stream.muted {
        (COLORS.overlay0, COLORS.overlay0, COLORS.overlay0)
    } else {
        (COLORS.text, COLORS.subtext0, view.accent)
    };
    text.draw_left(
        cr,
        (INNER_X, y, CHIP_H),
        &name,
        FONT.label,
        true,
        name_color,
    )?;
    let label = format!("{percent}%");
    text.draw_right(
        cr,
        (MUTE_X - COL_GAP, y, CHIP_H),
        &label,
        FONT.sm,
        false,
        percent_color,
    )?;
    let chip = BoxRect {
        x: MUTE_X,
        y,
        w: CHIP_W,
        h: CHIP_H,
    };
    let chip_hovered = hovered.is_some_and(|(_, on_chip)| on_chip);
    let chip_fill = if chip_hovered {
        COLORS.surface1
    } else {
        COLORS.surface0
    };
    draw_rect(cr, chip, chip_fill, COLORS.mantle, 0.0)?;
    let glyph = volume_style::level_glyph(percent, stream.muted).to_string();
    text.draw_centered(cr, chip, &glyph, FONT.heading, false, glyph_color)?;

    // The slider.
    let slider = BoxRect {
        x: INNER_X,
        y: slider_y(row) + (SLIDER_ROW_H - METER_H) / 2.0,
        w: INNER_W,
        h: METER_H,
    };
    let gradient = volume_style::meter_gradient(view.accent, stream.muted, percent);
    let fraction = f64::from(percent) / f64::from(SLIDER_MAX);
    meter_bar::draw(
        cr,
        slider,
        fraction,
        METER_SEGMENTS,
        &gradient,
        COLORS.surface0,
    )
}
