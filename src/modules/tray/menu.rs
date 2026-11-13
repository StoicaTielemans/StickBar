//! A tray item's right-click menu (the C bar's
//! `modules/tray/tray_context_menu.c`): the `DBusMenu` entries the `dbus`
//! thread fetched, as a dropdown under the icon.
//!
//! The look (`docs/modules/tray.md`): neutral grey (an `overlay0` border, an
//! `overlay2` hover bar), since the tray has no accent colour of its own. A
//! header with the item's icon, its name and the entry count; each run of
//! entries between separators is its own card (leading, trailing and doubled
//! separators make no empty card); a toggle column only when some entry has
//! a checkbox or radio; a chevron on submenu entries (shown, not openable).
//! Disabled entries are dimmed and don't react.

use std::fs::File;

use smithay_client_toolkit::reexports::client::protocol::wl_output::WlOutput;

use super::{Tray, draw_icon};
use crate::{
    bar::{
        popup_host::PopupHost,
        popup_surface::{PopupEnv, PopupSurface, render_image},
    },
    config::{
        COLORS, FONT,
        glyphs::{
            ICON_CHECKBOX_OFF, ICON_CHECKBOX_ON, ICON_CHEVRON_RIGHT, ICON_RADIO_OFF, ICON_RADIO_ON,
        },
    },
    dbus::{MenuEntry, Toggle, ToggleState},
    render::{
        BoxRect,
        module_box::draw_rect,
        popup_chrome,
        popup_style::{HEADER_H, HEADER_ICON_GAP, OUTER_MARGIN, SECTION_GAP},
        text::TextCache,
        whole_px,
    },
};

// Layout, local to this popup (CLAUDE.md "Conventions"); same header/card
// language as the other dropdowns.
/// The header icon's size: tray icons are already scaled to the bar's 20 px.
const HEADER_ICON: f64 = 20.0;
/// Between cards: tighter than `popup_style::CARD_GAP` (8), since menus run long.
const CARD_GAP: f64 = 6.0;
/// Card edge to its rows.
const CARD_PAD: f64 = 3.0;
/// One entry's height.
const ROW_H: f64 = 26.0;
/// Row edge to its text.
const ROW_PAD_X: f64 = 10.0;
/// The hover bar on a row's left edge.
const HOVER_BAR_W: f64 = 3.0;
/// Between a row's columns: tighter than `popup_style::COL_GAP` (10).
const COL_GAP: f64 = 8.0;
/// The narrowest the content gets.
const MIN_CONTENT_W: f64 = 200.0;

/// Where one entry's row is (separators have none).
#[derive(Clone, Copy, Debug)]
struct Row {
    /// Top, popup-local.
    y: f64,
}

/// An open menu.
struct Open {
    /// Its surfaces.
    host: PopupHost,
    /// What it shows, kept apart from `host` so a paint can borrow both.
    content: Content,
}

/// What an open menu shows, and where.
struct Content {
    /// Which tray item it belongs to.
    item: u64,
    /// The item's name, as it was when the menu opened ("Menu" without one).
    title: String,
    /// The entries, separators included.
    entries: Vec<MenuEntry>,
    /// Each entry's row (`None` for a separator).
    rows: Vec<Option<Row>>,
    /// Each card's (top, height).
    cards: Vec<(f64, f64)>,
    /// Non-separator entries, for the header.
    item_count: usize,
    /// Reserve the toggle column on every row.
    any_toggle: bool,
    /// The content column's width.
    width: f64,
    /// The popup's size.
    size: (f64, f64),
    /// The entry under the pointer.
    hovered: Option<usize>,
}

/// The tray menu: closed or open.
#[derive(Default)]
pub(crate) struct TrayMenu {
    /// The open menu, if any.
    open: Option<Open>,
}

impl TrayMenu {
    /// Opens `entries` (item `item`, called `title`) under the icon `trigger`
    /// on `output` (`output_width` px wide), replacing any open menu. Nothing
    /// opens for an empty menu, as in C.
    pub(crate) fn open<'o>(
        &mut self,
        env: &PopupEnv<'_>,
        (output, output_width, trigger): (&WlOutput, i32, BoxRect),
        all_outputs: impl Iterator<Item = &'o WlOutput>,
        (item, title, entries): (u64, &str, Vec<MenuEntry>),
        tray: &Tray,
        text: &mut TextCache,
    ) {
        self.open = None;
        if entries.is_empty() {
            return;
        }
        let title = if title.is_empty() { "Menu" } else { title }.to_owned();
        let layout = Layout::compute(&entries, &title, text);
        let host = PopupHost::open_right(
            env,
            (output, output_width),
            trigger,
            (whole_px(layout.size.0), whole_px(layout.size.1)),
            all_outputs,
        );
        self.open = Some(Open {
            host,
            content: Content {
                item,
                title,
                entries,
                rows: layout.rows,
                cards: layout.cards,
                item_count: layout.item_count,
                any_toggle: layout.any_toggle,
                width: layout.width,
                size: layout.size,
                hovered: None,
            },
        });
        self.paint(tray, text);
    }

    /// Closes the menu (no-op if closed).
    pub(crate) fn close(&mut self) {
        self.open = None;
    }

    /// The item whose menu is open.
    pub(crate) fn item(&self) -> Option<u64> {
        self.open.as_ref().map(|o| o.content.item)
    }

    /// The open menu's surfaces.
    pub(crate) fn host(&self) -> Option<&PopupHost> {
        self.open.as_ref().map(|o| &o.host)
    }

    /// The open menu's surfaces, for configure routing.
    pub(crate) fn host_mut(&mut self) -> Option<&mut PopupHost> {
        self.open.as_mut().map(|o| &mut o.host)
    }

    /// The menu surface, if open.
    pub(crate) fn menu(&self) -> Option<&PopupSurface> {
        self.host().map(PopupHost::menu)
    }

    /// The entries shown (the tests' `menu`), `None` when closed.
    pub(crate) fn entries(&self) -> Option<&[MenuEntry]> {
        self.open.as_ref().map(|o| o.content.entries.as_slice())
    }

    /// Paints the open menu.
    pub(crate) fn paint(&mut self, tray: &Tray, text: &mut TextCache) {
        if let Some(Open { host, content }) = self.open.as_mut() {
            host.menu_mut().paint(|cr| draw(cr, text, tray, content));
        }
    }

    /// The pointer moved to (x, y) on the menu: the hover highlight follows.
    pub(crate) fn motion(&mut self, (x, y): (f64, f64), tray: &Tray, text: &mut TextCache) {
        let Some(open) = self.open.as_mut() else {
            return;
        };
        let hovered = row_at(&open.content, x, y);
        if hovered != open.content.hovered {
            open.content.hovered = hovered;
            self.paint(tray, text);
        }
    }

    /// A press at (x, y) on the menu: the (item, entry id) to send a click
    /// for, if it landed on an enabled entry. The menu then closes; a press
    /// anywhere else (header, gaps, a disabled entry) is absorbed.
    pub(crate) fn press(&mut self, (x, y): (f64, f64)) -> Option<(u64, i32)> {
        let content = &self.open.as_ref()?.content;
        let entry = row_at(content, x, y).and_then(|i| content.entries.get(i))?;
        if !entry.enabled {
            return None;
        }
        let clicked = (content.item, entry.id);
        self.close();
        Some(clicked)
    }

    /// Whether a click at (x, y) would do something (the hand cursor).
    pub(crate) fn over_clickable(&self, (x, y): (f64, f64)) -> bool {
        self.open.as_ref().is_some_and(|o| {
            row_at(&o.content, x, y)
                .and_then(|i| o.content.entries.get(i))
                .is_some_and(|e| e.enabled)
        })
    }

    /// The middle of the row labelled `label`, menu-local (the tests'
    /// `target traymenu`).
    pub(crate) fn target(&self, label: &str) -> Option<(f64, f64)> {
        let content = &self.open.as_ref()?.content;
        let row = content
            .entries
            .iter()
            .zip(&content.rows)
            .find(|(e, _)| !e.separator && e.label == label)
            .and_then(|(_, row)| *row)?;
        Some((
            card_inner_x() + card_inner_w(content.width) / 2.0,
            row.y + ROW_H / 2.0,
        ))
    }

    /// Writes what the open menu shows, drawn again, to a PNG (the tests' `snapshot`).
    pub(crate) fn snapshot(
        &self,
        path: &str,
        tray: &Tray,
        text: &mut TextCache,
    ) -> Result<(), String> {
        let open = self.open.as_ref().ok_or("no open menu to snapshot")?;
        let (width, height) = open.host.menu().menu_size().ok_or("not a menu")?;
        let image = render_image(width, height, |cr| draw(cr, text, tray, &open.content))
            .map_err(|e| e.to_string())?;
        let mut file = File::create(path).map_err(|e| e.to_string())?;
        image.write_to_png(&mut file).map_err(|e| e.to_string())
    }
}

/// Where a card's rows start.
const fn card_inner_x() -> f64 {
    OUTER_MARGIN + CARD_PAD
}

/// A card's inner width, for content `width` wide.
fn card_inner_w(width: f64) -> f64 {
    width - CARD_PAD * 2.0
}

/// The entry whose row is under (x, y): not the header, the gaps between
/// cards, or a separator.
fn row_at(content: &Content, x: f64, y: f64) -> Option<usize> {
    let inner_x = card_inner_x();
    if !(inner_x..inner_x + card_inner_w(content.width)).contains(&x) {
        return None;
    }
    content
        .rows
        .iter()
        .position(|row| row.is_some_and(|r| (r.y..r.y + ROW_H).contains(&y)))
}

/// The toggle column's width: a checkbox glyph plus a gap.
fn toggle_col_w(text: &mut TextCache) -> f64 {
    text.width(&ICON_CHECKBOX_ON.to_string(), FONT.label, false) + COL_GAP
}

/// The submenu chevron's width.
fn chevron_w(text: &mut TextCache) -> f64 {
    text.width(&ICON_CHEVRON_RIGHT.to_string(), FONT.xs, false)
}

/// `  ·  N items`.
fn summary(item_count: usize) -> String {
    let plural = if item_count == 1 { "" } else { "s" };
    format!("  \u{b7}  {item_count} item{plural}")
}

/// The computed layout: worked out before the surface exists, since a popup
/// needs its size up front.
struct Layout {
    /// Each entry's row.
    rows: Vec<Option<Row>>,
    /// Each card's (top, height).
    cards: Vec<(f64, f64)>,
    /// Non-separator entries.
    item_count: usize,
    /// Some entry has a toggle.
    any_toggle: bool,
    /// The content column's width.
    width: f64,
    /// The popup's size.
    size: (f64, f64),
}

impl Layout {
    /// Cards, rows and size for `entries` under a header titled `title`.
    fn compute(entries: &[MenuEntry], title: &str, text: &mut TextCache) -> Self {
        let items: Vec<&MenuEntry> = entries.iter().filter(|e| !e.separator).collect();
        let item_count = items.len();
        let any_toggle = items.iter().any(|e| e.toggle.is_some());
        let chevron = chevron_w(text);
        let max_row_w = items
            .iter()
            .map(|e| {
                text.width(&e.label, FONT.label, false)
                    + if e.submenu { COL_GAP + chevron } else { 0.0 }
            })
            .fold(0.0, f64::max);
        let toggle_w = if any_toggle { toggle_col_w(text) } else { 0.0 };
        let row_w = ROW_PAD_X * 2.0 + toggle_w + max_row_w + CARD_PAD * 2.0;
        let header_w = HEADER_ICON
            + HEADER_ICON_GAP
            + text.width(title, FONT.md, true)
            + text.width(&summary(item_count), FONT.sm, false);
        let width = row_w.max(header_w).max(MIN_CONTENT_W);

        let mut y = OUTER_MARGIN + HEADER_H + SECTION_GAP;
        let mut rows = Vec::with_capacity(entries.len());
        let mut cards: Vec<(f64, f64)> = Vec::new();
        let mut in_card = false;
        for entry in entries {
            if entry.separator {
                if in_card && let Some(card) = cards.last_mut() {
                    card.1 = y + CARD_PAD - card.0;
                    y = card.0 + card.1 + CARD_GAP;
                    in_card = false;
                }
                rows.push(None);
                continue;
            }
            if !in_card {
                cards.push((y, 0.0));
                y += CARD_PAD;
                in_card = true;
            }
            rows.push(Some(Row { y }));
            y += ROW_H;
        }
        if in_card {
            if let Some(card) = cards.last_mut() {
                card.1 = y + CARD_PAD - card.0;
                y = card.0 + card.1;
            }
        } else if !cards.is_empty() {
            y -= CARD_GAP; // a trailing separator left one gap too many
        }
        Self {
            rows,
            cards,
            item_count,
            any_toggle,
            width,
            size: (width + OUTER_MARGIN * 2.0, y + OUTER_MARGIN),
        }
    }
}

/// The whole menu.
fn draw(
    cr: &cairo::Context,
    text: &mut TextCache,
    tray: &Tray,
    view: &Content,
) -> Result<(), cairo::Error> {
    popup_chrome::draw_frame(cr, view.size.0, view.size.1, COLORS.overlay0)?;

    // Header: the item's own tray icon, its name, and the entry count.
    let (x, y) = (OUTER_MARGIN, OUTER_MARGIN);
    let icon_rect = BoxRect {
        x,
        y: y + (HEADER_H - HEADER_ICON) / 2.0,
        w: HEADER_ICON,
        h: HEADER_ICON,
    };
    let icon = tray.item(view.item).and_then(|i| i.icon.as_ref());
    draw_icon(cr, icon, icon_rect)?;
    let mut tx = x + HEADER_ICON + HEADER_ICON_GAP;
    text.draw_left(
        cr,
        (tx, y, HEADER_H),
        &view.title,
        FONT.md,
        true,
        COLORS.text,
    )?;
    tx += text.width(&view.title, FONT.md, true);
    text.draw_left(
        cr,
        (tx, y, HEADER_H),
        &summary(view.item_count),
        FONT.sm,
        false,
        COLORS.subtext0,
    )?;

    for &(card_y, card_h) in &view.cards {
        let card = BoxRect {
            x: OUTER_MARGIN,
            y: card_y,
            w: view.width,
            h: card_h,
        };
        popup_chrome::draw_card(cr, card, false, false, COLORS.surface1)?;
    }
    for (i, (entry, row)) in view.entries.iter().zip(&view.rows).enumerate() {
        if let Some(row) = row {
            draw_row(cr, text, view, (entry, *row), view.hovered == Some(i))?;
        }
    }
    Ok(())
}

/// One entry's row.
fn draw_row(
    cr: &cairo::Context,
    text: &mut TextCache,
    view: &Content,
    (entry, row): (&MenuEntry, Row),
    hovered: bool,
) -> Result<(), cairo::Error> {
    let (x, w) = (card_inner_x(), card_inner_w(view.width));
    // Disabled entries are dimmed and never highlight (they can't be clicked).
    if hovered && entry.enabled {
        let fill = BoxRect {
            x,
            y: row.y,
            w,
            h: ROW_H,
        };
        draw_rect(cr, fill, COLORS.surface0, COLORS.mantle, 0.0)?;
        let bar = BoxRect {
            w: HOVER_BAR_W,
            ..fill
        };
        draw_rect(cr, bar, COLORS.overlay2, COLORS.mantle, 0.0)?;
    }
    let text_color = if entry.enabled {
        COLORS.text
    } else {
        COLORS.overlay0
    };
    let mut tx = x + ROW_PAD_X;
    if view.any_toggle {
        if let Some(toggle) = entry.toggle {
            let ToggleState { kind, on } = toggle;
            let glyph = match (kind, on) {
                (Toggle::Radio, true) => ICON_RADIO_ON,
                (Toggle::Radio, false) => ICON_RADIO_OFF,
                (Toggle::Checkmark, true) => ICON_CHECKBOX_ON,
                (Toggle::Checkmark, false) => ICON_CHECKBOX_OFF,
            };
            let color = if on && entry.enabled {
                COLORS.text
            } else {
                COLORS.overlay0
            };
            text.draw_left(
                cr,
                (tx, row.y, ROW_H),
                &glyph.to_string(),
                FONT.label,
                false,
                color,
            )?;
        }
        tx += toggle_col_w(text);
    }
    text.draw_left(
        cr,
        (tx, row.y, ROW_H),
        &entry.label,
        FONT.label,
        false,
        text_color,
    )?;
    if entry.submenu {
        // Shown so the entry doesn't look like a plain action; opening
        // submenus isn't supported (docs/limitations.md).
        let cx = x + w - ROW_PAD_X - chevron_w(text);
        text.draw_left(
            cr,
            (cx, row.y, ROW_H),
            &ICON_CHEVRON_RIGHT.to_string(),
            FONT.xs,
            false,
            COLORS.overlay1,
        )?;
    }
    Ok(())
}
