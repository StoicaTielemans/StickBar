//! The updates dropdown (`docs/modules/updates.md`): the package icon,
//! `Updates · N packages` and a pill (`checking` yellow, `failed` red, or how
//! long ago the last check was, grey), an error line if the last check
//! failed, then one card per source (`REPO`, `AUR`) with one row per package:
//! the name on the left, `old → new` on the right.
//!
//! At most [`MAX_ROWS`] package rows in all, so 50+ pending updates don't make
//! a screen-tall popup: the AUR card gets up to [`MAX_AUR_ROWS`] (it's usually
//! short, and the one you'd look for), the repo card the rest, and a card
//! that's cut short ends with `+N more`. Opened on right-click (anchored under
//! the box's right edge, like the other right-region dropdowns); nothing
//! inside is clickable. A finished check re-lays it out (recreating the
//! surface if the height changed); with nothing left to update it closes.

use std::fs::File;

use smithay_client_toolkit::reexports::client::protocol::wl_output::WlOutput;

use crate::{
    bar::{
        popup_host::PopupHost,
        popup_surface::{PopupEnv, PopupSurface, render_image},
    },
    config::{COLORS, Color, FONT, glyphs::ICON_UPDATES},
    render::{
        BoxRect,
        popup_chrome::{self, Header},
        popup_style::{CARD_GAP, COL_GAP, HEADER_H, OUTER_MARGIN, SECTION_GAP},
        text::TextCache,
        whole_px,
    },
    updates::{Pkg, Updates},
};

// Layout, local to this popup (CLAUDE.md "Conventions").
/// The content column's width (fixed: long names are shortened instead).
const CONTENT_W: f64 = 420.0;
/// Card edge to its content.
const CARD_PAD: f64 = 8.0;
/// A card's title row (`REPO` and its count).
const CARD_TITLE_H: f64 = 18.0;
/// One package's row.
const ROW_H: f64 = 18.0;
/// The error line under the header, when the last check failed.
const ERROR_ROW_H: f64 = 16.0;
/// The most package rows shown, both cards together.
pub(crate) const MAX_ROWS: usize = 30;
/// The most AUR rows shown (the rest of [`MAX_ROWS`] goes to the repo card).
const MAX_AUR_ROWS: usize = 10;
/// The dropdown's accent: the box's.
const ACCENT: Color = COLORS.pink;

/// How many rows each card shows: `(repo, aur)`.
pub(crate) fn shown_rows(repo: usize, aur: usize) -> (usize, usize) {
    let aur_shown = aur.min(MAX_AUR_ROWS);
    (repo.min(MAX_ROWS - aur_shown), aur_shown)
}

/// One source's card, as laid out.
#[derive(Clone, Copy, Debug)]
struct Card {
    /// `REPO` or `AUR`.
    title: &'static str,
    /// Which list it shows.
    aur: bool,
    /// Its top.
    y: f64,
    /// Package rows shown.
    rows: usize,
    /// Packages not shown (the `+N more` row when > 0).
    more: usize,
}

impl Card {
    /// Its height.
    fn height(self) -> f64 {
        let extra = usize::from(self.more > 0);
        // At most MAX_ROWS + 1 rows: the conversion can't fail.
        let rows = f64::from(u32::try_from(self.rows + extra).unwrap_or(0));
        CARD_PAD * 2.0 + CARD_TITLE_H + rows * ROW_H
    }
}

/// An open dropdown.
struct Open {
    /// Its surfaces.
    host: PopupHost,
    /// Its cards.
    cards: Vec<Card>,
    /// Its size.
    size: (f64, f64),
}

/// The updates dropdown: closed or open.
#[derive(Default)]
pub(crate) struct UpdatesPopup {
    /// The open dropdown, if any.
    open: Option<Open>,
}

impl UpdatesPopup {
    /// Opens the dropdown under the box `trigger` on `output` (`output_width`
    /// px wide), replacing an open one. Nothing opens with nothing to update.
    pub(crate) fn open<'o>(
        &mut self,
        env: &PopupEnv<'_>,
        (output, output_width): (&WlOutput, i32),
        trigger: BoxRect,
        all_outputs: impl Iterator<Item = &'o WlOutput>,
        updates: &Updates,
        text: &mut TextCache,
    ) {
        self.open = None;
        if updates.total() == 0 {
            return;
        }
        let (cards, size) = layout(updates);
        let host = PopupHost::open_right(
            env,
            (output, output_width),
            trigger,
            (whole_px(size.0), whole_px(size.1)),
            all_outputs,
        );
        self.open = Some(Open { host, cards, size });
        self.paint(updates, text);
    }

    /// A check finished: re-lays out, recreating the surface if the height
    /// changed (as the media dropdown does), and closes with nothing left.
    pub(crate) fn refresh(&mut self, env: &PopupEnv<'_>, updates: &Updates, text: &mut TextCache) {
        let Some(open) = self.open.as_mut() else {
            return;
        };
        if updates.total() == 0 {
            self.open = None;
            return;
        }
        let (cards, size) = layout(updates);
        if whole_px(size.1) != whole_px(open.size.1) {
            open.host.resize(env, (whole_px(size.0), whole_px(size.1)));
        }
        open.cards = cards;
        open.size = size;
        self.paint(updates, text);
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

    /// How many package rows are shown (the tests' `menu`), `None` when closed.
    pub(crate) fn row_count(&self) -> Option<usize> {
        self.open
            .as_ref()
            .map(|o| o.cards.iter().map(|c| c.rows).sum())
    }

    /// Paints the open dropdown.
    pub(crate) fn paint(&mut self, updates: &Updates, text: &mut TextCache) {
        if let Some(Open { host, cards, size }) = self.open.as_mut() {
            let (cards, size) = (cards.as_slice(), *size);
            host.menu_mut()
                .paint(|cr| draw(cr, text, updates, (cards, size)));
        }
    }

    /// Writes what the open dropdown shows, drawn again, to a PNG (the tests'
    /// `snapshot`).
    pub(crate) fn snapshot(
        &self,
        path: &str,
        updates: &Updates,
        text: &mut TextCache,
    ) -> Result<(), String> {
        let open = self.open.as_ref().ok_or("no open menu to snapshot")?;
        let (width, height) = open.host.menu().menu_size().ok_or("not a menu")?;
        let image = render_image(width, height, |cr| {
            draw(cr, text, updates, (&open.cards, open.size))
        })
        .map_err(|e| e.to_string())?;
        let mut file = File::create(path).map_err(|e| e.to_string())?;
        image.write_to_png(&mut file).map_err(|e| e.to_string())
    }
}

/// The cards for `updates` (an empty source left out) and the popup's size.
fn layout(updates: &Updates) -> (Vec<Card>, (f64, f64)) {
    let mut y = OUTER_MARGIN + HEADER_H + SECTION_GAP;
    if updates.last_error.is_some() {
        y += ERROR_ROW_H + CARD_GAP;
    }
    let (repo_rows, aur_rows) = shown_rows(updates.repo.len(), updates.aur.len());
    let mut cards = Vec::new();
    for (title, aur, total, rows) in [
        ("REPO", false, updates.repo.len(), repo_rows),
        ("AUR", true, updates.aur.len(), aur_rows),
    ] {
        if total == 0 {
            continue;
        }
        let card = Card {
            title,
            aur,
            y,
            rows,
            more: total - rows,
        };
        y += card.height() + CARD_GAP;
        cards.push(card);
    }
    // The last card's gap isn't needed.
    y -= CARD_GAP;
    (cards, (CONTENT_W + OUTER_MARGIN * 2.0, y + OUTER_MARGIN))
}

/// The header pill: a check running, the last one failed, or how long ago
/// the last good one was.
fn pill(updates: &Updates) -> (String, Color) {
    if updates.checking {
        return ("checking".to_owned(), COLORS.yellow);
    }
    if updates.last_error.is_some() {
        return ("failed".to_owned(), COLORS.red);
    }
    let minutes = updates.last_ok.map_or(0, |t| t.elapsed().as_secs() / 60);
    let ago = match minutes {
        0 => "just now".to_owned(),
        m if m < 60 => format!("{m} min ago"),
        m => format!("{} h ago", m / 60),
    };
    (ago, COLORS.overlay1)
}

/// The whole dropdown.
fn draw(
    cr: &cairo::Context,
    text: &mut TextCache,
    updates: &Updates,
    (cards, size): (&[Card], (f64, f64)),
) -> Result<(), cairo::Error> {
    popup_chrome::draw_frame(cr, size.0, size.1, ACCENT)?;
    let n = updates.total();
    let plural = if n == 1 { "" } else { "s" };
    let summary = format!("  \u{b7}  {n} package{plural}");
    let (word, fill) = pill(updates);
    let header = Header {
        icon: Some(ICON_UPDATES),
        icon_color: ACCENT,
        title: "Updates",
        summary: &summary,
        pill: Some(&word),
        pill_fill: fill,
    };
    popup_chrome::draw_header(cr, text, (OUTER_MARGIN, OUTER_MARGIN, CONTENT_W), &header)?;
    if let Some(error) = &updates.last_error {
        let line = text.truncate(&format!("last check: {error}"), CONTENT_W, FONT.sm, false);
        text.draw_left(
            cr,
            (
                OUTER_MARGIN,
                OUTER_MARGIN + HEADER_H + SECTION_GAP,
                ERROR_ROW_H,
            ),
            &line,
            FONT.sm,
            false,
            COLORS.red,
        )?;
    }
    for card in cards {
        let list = if card.aur {
            &updates.aur
        } else {
            &updates.repo
        };
        draw_card(cr, text, *card, list)?;
    }
    Ok(())
}

/// One source's card.
fn draw_card(
    cr: &cairo::Context,
    text: &mut TextCache,
    card: Card,
    list: &[Pkg],
) -> Result<(), cairo::Error> {
    let rect = BoxRect {
        x: OUTER_MARGIN,
        y: card.y,
        w: CONTENT_W,
        h: card.height(),
    };
    popup_chrome::draw_card(cr, rect, false, false, ACCENT)?;
    let inner_x = OUTER_MARGIN + CARD_PAD;
    let inner_w = CONTENT_W - CARD_PAD * 2.0;
    let mut y = card.y + CARD_PAD;
    text.draw_left(
        cr,
        (inner_x, y, CARD_TITLE_H),
        card.title,
        FONT.xs,
        true,
        COLORS.overlay0,
    )?;
    text.draw_right(
        cr,
        (inner_x + inner_w, y, CARD_TITLE_H),
        &list.len().to_string(),
        FONT.xs,
        true,
        COLORS.overlay0,
    )?;
    y += CARD_TITLE_H;
    for pkg in list.iter().take(card.rows) {
        // The versions are what's new, so they're kept whole; a long name is
        // shortened short of them.
        let versions = format!("{} \u{2192} {}", pkg.old, pkg.new);
        let versions_w = text.width(&versions, FONT.sm, false);
        text.draw_right(
            cr,
            (inner_x + inner_w, y, ROW_H),
            &versions,
            FONT.sm,
            false,
            COLORS.subtext0,
        )?;
        let name = text.truncate(&pkg.name, inner_w - versions_w - COL_GAP, FONT.md, false);
        text.draw_left(cr, (inner_x, y, ROW_H), &name, FONT.md, false, COLORS.text)?;
        y += ROW_H;
    }
    if card.more > 0 {
        text.draw_left(
            cr,
            (inner_x, y, ROW_H),
            &format!("+{} more", card.more),
            FONT.sm,
            false,
            COLORS.overlay1,
        )?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{MAX_ROWS, shown_rows};

    #[test]
    fn rows_are_capped_with_room_kept_for_the_aur() {
        assert_eq!(shown_rows(5, 1), (5, 1));
        assert_eq!(shown_rows(53, 1), (MAX_ROWS - 1, 1));
        assert_eq!(shown_rows(53, 40), (MAX_ROWS - 10, 10));
        assert_eq!(shown_rows(0, 40), (0, 10));
    }
}
