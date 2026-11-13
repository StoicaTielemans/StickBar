//! The privacy dropdown (the C bar's `modules/privacy/privacy_info_popup.c`):
//! a shield, `Privacy · N streams` and a maroon `in use` pill, then one card
//! per kind of recording (MICROPHONE / WEBCAM / SCREEN SHARE, an empty kind
//! left out) with its icon, title and count, and one app name per line.
//!
//! The streams are a snapshot taken at open, so the width can follow the
//! longest name (260-480 px; past that a name is clipped at the card's edge)
//! without ever having to change while it's shown. Nothing inside is clickable.

use std::fs::File;

use smithay_client_toolkit::reexports::client::protocol::wl_output::WlOutput;

use super::{CATEGORIES, glyph};
use crate::{
    bar::{
        popup_host::PopupHost,
        popup_surface::{PopupEnv, PopupSurface, render_image},
    },
    config::{COLORS, FONT, glyphs::ICON_PRIVACY},
    pipewire::{Category, PrivacyState},
    render::{
        BoxRect,
        popup_chrome::{self, Header},
        popup_style::{CARD_GAP, HEADER_H, OUTER_MARGIN, SECTION_GAP},
        text::TextCache,
        whole_px,
    },
};

// Layout, local to this popup (CLAUDE.md "Conventions").
/// The narrowest the content gets.
const MIN_CONTENT_W: f64 = 260.0;
/// The widest the content gets (longer names are clipped).
const MAX_CONTENT_W: f64 = 480.0;
/// Card edge to its content.
const CARD_PAD: f64 = 8.0;
/// A card's title row.
const CARD_TITLE_H: f64 = 18.0;
/// The title's icon to its text.
const CARD_TITLE_ICON_GAP: f64 = 6.0;
/// One app name's row.
const APP_ROW_H: f64 = 18.0;

/// What an open dropdown shows.
struct Content {
    /// The streams, as they were at open.
    state: PrivacyState,
    /// The content column's width.
    width: f64,
    /// The popup's size.
    size: (f64, f64),
}

/// An open dropdown.
struct Open {
    /// Its surfaces.
    host: PopupHost,
    /// What it shows.
    content: Content,
}

/// The privacy dropdown: closed or open.
#[derive(Default)]
pub(crate) struct PrivacyPopup {
    /// The open dropdown, if any.
    open: Option<Open>,
}

impl PrivacyPopup {
    /// Opens the dropdown under the box `trigger` on `output` with the
    /// streams in `state`, replacing an open one.
    pub(crate) fn open<'o>(
        &mut self,
        env: &PopupEnv<'_>,
        (output, trigger): (&WlOutput, BoxRect),
        all_outputs: impl Iterator<Item = &'o WlOutput>,
        state: &PrivacyState,
        text: &mut TextCache,
    ) {
        self.open = None;
        let width = state
            .entries
            .iter()
            .map(|(_, name)| text.width(name, FONT.md, false) + CARD_PAD * 2.0)
            .fold(MIN_CONTENT_W, f64::max)
            .min(MAX_CONTENT_W);
        let mut y = OUTER_MARGIN + HEADER_H + SECTION_GAP;
        let mut any = false;
        for category in CATEGORIES {
            let n = count(state, category);
            if n > 0 {
                y += if any { CARD_GAP } else { 0.0 } + card_height(n);
                any = true;
            }
        }
        let size = (width + OUTER_MARGIN * 2.0, y + OUTER_MARGIN);
        let host = PopupHost::open_left(
            env,
            output,
            trigger,
            (whole_px(size.0), whole_px(size.1)),
            all_outputs,
        );
        self.open = Some(Open {
            host,
            content: Content {
                state: state.clone(),
                width,
                size,
            },
        });
        self.paint(text);
    }

    /// Closes the dropdown (no-op if closed).
    pub(crate) fn close(&mut self) {
        self.open = None;
    }

    /// The open dropdown's surfaces.
    pub(crate) fn host(&self) -> Option<&PopupHost> {
        self.open.as_ref().map(|o| &o.host)
    }

    /// The open dropdown's surfaces, for configure routing.
    pub(crate) fn host_mut(&mut self) -> Option<&mut PopupHost> {
        self.open.as_mut().map(|o| &mut o.host)
    }

    /// The menu surface, if open.
    pub(crate) fn menu(&self) -> Option<&PopupSurface> {
        self.host().map(PopupHost::menu)
    }

    /// The streams shown (the tests' `menu`), `None` when closed.
    pub(crate) fn entries(&self) -> Option<&[(Category, String)]> {
        self.open
            .as_ref()
            .map(|o| o.content.state.entries.as_slice())
    }

    /// Paints the open dropdown.
    pub(crate) fn paint(&mut self, text: &mut TextCache) {
        if let Some(Open { host, content }) = self.open.as_mut() {
            host.menu_mut().paint(|cr| draw(cr, text, content));
        }
    }

    /// Writes what the open dropdown shows, drawn again, to a PNG (the tests'
    /// `snapshot`).
    pub(crate) fn snapshot(&self, path: &str, text: &mut TextCache) -> Result<(), String> {
        let open = self.open.as_ref().ok_or("no open menu to snapshot")?;
        let (width, height) = open.host.menu().menu_size().ok_or("not a menu")?;
        let image = render_image(width, height, |cr| draw(cr, text, &open.content))
            .map_err(|e| e.to_string())?;
        let mut file = File::create(path).map_err(|e| e.to_string())?;
        image.write_to_png(&mut file).map_err(|e| e.to_string())
    }
}

/// A kind's title, as the tests read it.
pub(crate) const fn title(category: Category) -> &'static str {
    match category {
        Category::Mic => "MICROPHONE",
        Category::Webcam => "WEBCAM",
        Category::ScreenShare => "SCREEN SHARE",
    }
}

/// How many streams are of kind `category`.
fn count(state: &PrivacyState, category: Category) -> usize {
    state.entries.iter().filter(|(c, _)| *c == category).count()
}

/// A card's height for `n` names.
fn card_height(n: usize) -> f64 {
    // A handful of names: the conversion can't fail.
    let rows = f64::from(u32::try_from(n).unwrap_or(0));
    CARD_PAD * 2.0 + CARD_TITLE_H + rows * APP_ROW_H
}

/// The whole dropdown.
fn draw(cr: &cairo::Context, text: &mut TextCache, content: &Content) -> Result<(), cairo::Error> {
    popup_chrome::draw_frame(cr, content.size.0, content.size.1, COLORS.maroon)?;
    let n = content.state.entries.len();
    let plural = if n == 1 { "" } else { "s" };
    let summary = format!("  \u{b7}  {n} stream{plural}");
    let header = Header {
        icon: Some(ICON_PRIVACY),
        icon_color: COLORS.maroon,
        title: "Privacy",
        summary: &summary,
        pill: Some("in use"),
        pill_fill: COLORS.maroon,
    };
    popup_chrome::draw_header(
        cr,
        text,
        (OUTER_MARGIN, OUTER_MARGIN, content.width),
        &header,
    )?;
    let mut y = OUTER_MARGIN + HEADER_H + SECTION_GAP;
    for category in CATEGORIES {
        let names = count(&content.state, category);
        if names > 0 {
            y = draw_card(cr, text, content, (category, names), y)? + CARD_GAP;
        }
    }
    Ok(())
}

/// One kind's card at `y`. Returns its bottom edge.
fn draw_card(
    cr: &cairo::Context,
    text: &mut TextCache,
    content: &Content,
    (category, n): (Category, usize),
    y: f64,
) -> Result<f64, cairo::Error> {
    let h = card_height(n);
    let card = BoxRect {
        x: OUTER_MARGIN,
        y,
        w: content.width,
        h,
    };
    popup_chrome::draw_card(cr, card, false, false, COLORS.surface1)?;

    let inner_x = OUTER_MARGIN + CARD_PAD;
    let inner_w = content.width - CARD_PAD * 2.0;
    let mut row_y = y + CARD_PAD;
    let icon = glyph(category).to_string();
    text.draw_left(
        cr,
        (inner_x, row_y, CARD_TITLE_H),
        &icon,
        FONT.md,
        false,
        COLORS.maroon,
    )?;
    let title_x = inner_x + text.width(&icon, FONT.md, false) + CARD_TITLE_ICON_GAP;
    text.draw_left(
        cr,
        (title_x, row_y, CARD_TITLE_H),
        title(category),
        FONT.xs,
        true,
        COLORS.overlay0,
    )?;
    text.draw_right(
        cr,
        (inner_x + inner_w, row_y, CARD_TITLE_H),
        &n.to_string(),
        FONT.xs,
        true,
        COLORS.overlay0,
    )?;
    row_y += CARD_TITLE_H;

    // Clipped to the card: a name past the widest width is cut at its edge
    // rather than drawn over the popup's border.
    cr.save()?;
    cr.rectangle(inner_x, y, inner_w, h);
    cr.clip();
    for (_, name) in content.state.entries.iter().filter(|(c, _)| *c == category) {
        text.draw_left(
            cr,
            (inner_x, row_y, APP_ROW_H),
            name,
            FONT.md,
            false,
            COLORS.text,
        )?;
        row_y += APP_ROW_H;
    }
    cr.restore()?;
    Ok(y + h)
}
