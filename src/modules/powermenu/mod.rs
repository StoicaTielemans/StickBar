//! The power menu (the C bar's `modules/powermenu/power_menu.c`; the full
//! story is in `docs/modules/power-menu.md`).
//!
//! Opened by a left click on the Arch logo or by the Hyprland keybind
//! (`pkill -SIGUSR1 -x stickBar`, handled in `main.rs`), and both toggle it. On
//! every monitor it puts a full-screen surface on the overlay layer: a 30%
//! crust dim with a centered panel. The panel has a header ("Power ·
//! user@host" and an uptime pill) and three cards: Logout, Restart, Shutdown.
//! A hovered card lightens and takes its action's colour.
//!
//! The full-screen surface is its own click-catcher. A click on a card closes
//! the menu and picks that action; a click elsewhere on the panel is absorbed;
//! a click on the dim just closes it. **Running** the action is the caller's
//! job, and a test-control instance never does (`wayland/pointer.rs`): tests
//! click every card, and that must never log out or reboot the machine.

pub(crate) mod arch_logo;

use std::fs::File;

use smithay_client_toolkit::reexports::client::protocol::{
    wl_output::WlOutput, wl_surface::WlSurface,
};

use crate::{
    bar::popup_surface::{PopupEnv, PopupSurface, render_image},
    config::{
        BAR, COLORS, Color, FONT,
        glyphs::{ICON_LOGOUT, ICON_POWER, ICON_RESTART},
    },
    render::{
        BoxRect,
        module_box::draw_rect,
        popup_chrome::{self, Header},
        popup_style::{CARD_GAP, HEADER_H, OUTER_MARGIN, SECTION_GAP},
        text::TextCache,
        whole_px,
    },
};

/// One action card's width.
const CARD_W: f64 = 116.0;
/// One action card's height.
const CARD_H: f64 = 112.0;
/// Card icon to its name.
const ICON_LABEL_GAP: f64 = 4.0;
/// Card name to its one-line description.
const LABEL_DETAIL_GAP: f64 = 2.0;
/// The row of three cards.
const CONTENT_W: f64 = 3.0 * CARD_W + 2.0 * CARD_GAP;
/// The panel, margins included (388 px).
const PANEL_W: f64 = CONTENT_W + OUTER_MARGIN * 2.0;
/// The panel, margins included (168 px).
const PANEL_H: f64 = OUTER_MARGIN + HEADER_H + SECTION_GAP + CARD_H + OUTER_MARGIN;
/// The dim's opacity: the reference's `#4D` alpha prefix (0x4D / 255).
const DIM_ALPHA: f64 = 0.302;

/// What a card does.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum PowerAction {
    /// End the Hyprland session.
    Logout,
    /// Reboot the machine.
    Restart,
    /// Power it off.
    Shutdown,
}

impl PowerAction {
    /// The name the tests know it by (`menu`'s `power_last_action`).
    const fn name(self) -> &'static str {
        match self {
            Self::Logout => "logout",
            Self::Restart => "restart",
            Self::Shutdown => "shutdown",
        }
    }
}

/// One card's content.
struct Card {
    /// What it does.
    action: PowerAction,
    /// Its colour: the icon always, the border and name when hovered.
    accent: Color,
    /// Its big icon.
    icon: char,
    /// Its name.
    label: &'static str,
    /// Its one-line description.
    detail: &'static str,
}

/// The three cards, left to right.
const CARDS: [Card; 3] = [
    Card {
        action: PowerAction::Logout,
        accent: COLORS.yellow,
        icon: ICON_LOGOUT,
        label: "Logout",
        detail: "End this session",
    },
    Card {
        action: PowerAction::Restart,
        accent: COLORS.sky,
        icon: ICON_RESTART,
        label: "Restart",
        detail: "Reboot the machine",
    },
    Card {
        action: PowerAction::Shutdown,
        accent: COLORS.red,
        icon: ICON_POWER,
        label: "Shutdown",
        detail: "Power off",
    },
];

/// Where the panel and its cards sit on one monitor's surface.
#[derive(Clone, Copy, Debug)]
struct Layout {
    /// The panel.
    panel: BoxRect,
    /// The three cards, in `CARDS` order.
    cards: [BoxRect; 3],
}

impl Layout {
    /// The layout on a `width` x `height` surface: the panel centered on the
    /// surface itself, not the monitor. The surface stops at the bars'
    /// reserved strips, so centering by the monitor's height put the panel too
    /// low (BUG.md, "Panel sat too low").
    fn for_size((width, height): (i32, i32)) -> Self {
        let panel = BoxRect {
            x: ((f64::from(width) - PANEL_W) / 2.0).floor(),
            y: ((f64::from(height) - PANEL_H) / 2.0).floor(),
            w: PANEL_W,
            h: PANEL_H,
        };
        let card = |column: f64| BoxRect {
            x: panel.x + OUTER_MARGIN + column * (CARD_W + CARD_GAP),
            y: panel.y + OUTER_MARGIN + HEADER_H + SECTION_GAP,
            w: CARD_W,
            h: CARD_H,
        };
        Self {
            panel,
            cards: [card(0.0), card(1.0), card(2.0)],
        }
    }

    /// Which card (index into `CARDS`) is at (x, y), if any.
    fn card_at(&self, x: f64, y: f64) -> Option<usize> {
        self.cards.iter().position(|card| card.contains(x, y))
    }
}

/// The result of a click on one of the menu's surfaces.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum PowerClick {
    /// The surface isn't the power menu's.
    NotOurs,
    /// On the panel but not a card: absorbed, the menu stays open.
    Absorbed,
    /// The menu closed; with an action if a card was clicked.
    Closed(Option<PowerAction>),
}

/// The power menu: closed (`overlays` empty) or open on every monitor.
#[derive(Default)]
pub(crate) struct PowerMenu {
    /// One full-screen surface per monitor while open.
    overlays: Vec<PopupSurface>,
    /// The hovered card: (overlay index, card index).
    hovered: Option<(usize, usize)>,
    /// "user@host", read when the menu opens.
    identity: String,
    /// e.g. "up 2d 3h", read when the menu opens; `None` if unreadable.
    uptime: Option<String>,
    /// The last card picked, for the tests' `menu` command ("" if none yet).
    last_action: &'static str,
}

impl PowerMenu {
    /// Whether the menu is showing.
    pub(crate) fn is_open(&self) -> bool {
        !self.overlays.is_empty()
    }

    /// Opens the menu on each of `outputs`, or closes it if open.
    pub(crate) fn toggle<'o>(
        &mut self,
        env: &PopupEnv<'_>,
        outputs: impl Iterator<Item = &'o WlOutput>,
        text: &mut TextCache,
    ) {
        if self.is_open() {
            self.close();
            return;
        }
        self.identity = identity();
        self.uptime = uptime();
        self.hovered = None;
        self.overlays = outputs
            .map(|output| PopupSurface::overlay(env, output))
            .collect();
        // Not configured yet, so this only marks each one as wanting a paint;
        // the real paint happens at each surface's first configure.
        self.paint_all(text);
    }

    /// Closes the menu (no-op if closed).
    pub(crate) fn close(&mut self) {
        self.overlays.clear();
        self.hovered = None;
    }

    /// The overlay surfaces, for configure routing and the tests.
    pub(crate) fn surfaces(&self) -> &[PopupSurface] {
        &self.overlays
    }

    /// The overlay whose surface is `surface`, for configure routing.
    pub(crate) fn surface_mut(&mut self, surface: &WlSurface) -> Option<&mut PopupSurface> {
        self.overlays.iter_mut().find(|o| o.wl_surface() == surface)
    }

    /// Which overlay `surface` is, if it's one of the menu's.
    fn overlay_index(&self, surface: &WlSurface) -> Option<usize> {
        self.overlays.iter().position(|o| o.wl_surface() == surface)
    }

    /// A click (any button) at (x, y) on `surface`. See [`PowerClick`]. The
    /// picked action is also remembered for the tests.
    pub(crate) fn click(&mut self, surface: &WlSurface, x: f64, y: f64) -> PowerClick {
        let Some(overlay) = self.overlays.iter().find(|o| o.wl_surface() == surface) else {
            return PowerClick::NotOurs;
        };
        let layout = Layout::for_size(overlay.size());
        let card = layout.card_at(x, y);
        if card.is_none() && layout.panel.contains(x, y) {
            return PowerClick::Absorbed;
        }
        let action = card.and_then(|i| CARDS.get(i)).map(|c| c.action);
        self.close();
        if let Some(action) = action {
            self.last_action = action.name();
        }
        PowerClick::Closed(action)
    }

    /// The pointer moved to (x, y) on `surface`. Returns false if `surface`
    /// isn't the menu's.
    pub(crate) fn motion(
        &mut self,
        surface: &WlSurface,
        x: f64,
        y: f64,
        text: &mut TextCache,
    ) -> bool {
        let Some(index) = self.overlay_index(surface) else {
            return false;
        };
        let card = self
            .overlays
            .get(index)
            .and_then(|o| Layout::for_size(o.size()).card_at(x, y));
        let hovered = card.map(|card| (index, card));
        if hovered != self.hovered {
            self.hovered = hovered;
            self.paint_all(text);
        }
        true
    }

    /// Whether (x, y) on `surface` is a card, for the hand cursor.
    pub(crate) fn is_card(&self, surface: &WlSurface, x: f64, y: f64) -> bool {
        self.overlays
            .iter()
            .find(|o| o.wl_surface() == surface)
            .and_then(|o| Layout::for_size(o.size()).card_at(x, y))
            .is_some()
    }

    /// The last card picked ("" if none), for the tests' `menu` command.
    pub(crate) const fn last_action(&self) -> &'static str {
        self.last_action
    }

    /// A point on the first monitor's panel, for the tests' `target power
    /// <part>`: a card's centre (`logout`/`restart`/`shutdown`), the panel's
    /// top-left `corner`, or the middle of its `header`.
    pub(crate) fn target(&self, part: &str) -> Option<(f64, f64)> {
        let layout = Layout::for_size(self.overlays.first()?.size());
        let card = |i: usize| {
            layout
                .cards
                .get(i)
                .map(|c| (c.x + c.w / 2.0, c.y + c.h / 2.0))
        };
        match part {
            "logout" => card(0),
            "restart" => card(1),
            "shutdown" => card(2),
            "corner" => Some((layout.panel.x, layout.panel.y)),
            "header" => Some((
                layout.panel.x + PANEL_W / 2.0,
                layout.panel.y + OUTER_MARGIN + HEADER_H / 2.0,
            )),
            _ => None,
        }
    }

    /// Repaints every monitor's overlay (each at its own size).
    pub(crate) fn paint_all(&mut self, text: &mut TextCache) {
        let Self {
            overlays,
            hovered,
            identity,
            uptime,
            ..
        } = self;
        for (index, overlay) in overlays.iter_mut().enumerate() {
            let layout = Layout::for_size(overlay.size());
            let hovered_card = hovered.filter(|(o, _)| *o == index).map(|(_, card)| card);
            let header = HeaderText {
                identity,
                uptime: uptime.as_deref(),
            };
            overlay.paint(|cr| draw(cr, text, &layout, hovered_card, &header));
        }
    }

    /// Writes the first monitor's overlay, drawn again, to a PNG (the tests'
    /// `snapshot`).
    pub(crate) fn snapshot(&self, path: &str, text: &mut TextCache) -> Result<(), String> {
        let overlay = self.overlays.first().ok_or("no open menu to snapshot")?;
        let (width, height) = overlay.size();
        if width <= 0 || height <= 0 {
            return Err("the power menu isn't configured yet".to_owned());
        }
        let layout = Layout::for_size((width, height));
        let hovered_card = self.hovered.filter(|(o, _)| *o == 0).map(|(_, card)| card);
        let header = HeaderText {
            identity: &self.identity,
            uptime: self.uptime.as_deref(),
        };
        let image = render_image(width, height, |cr| {
            draw(cr, text, &layout, hovered_card, &header)
        })
        .map_err(|e| e.to_string())?;
        let mut file = File::create(path).map_err(|e| e.to_string())?;
        image.write_to_png(&mut file).map_err(|e| e.to_string())
    }
}

/// The header's variable text, read when the menu opened.
struct HeaderText<'a> {
    /// "user@host".
    identity: &'a str,
    /// e.g. "up 2d 3h".
    uptime: Option<&'a str>,
}

/// Draws one monitor's overlay: dim, panel, header, cards.
fn draw(
    cr: &cairo::Context,
    text: &mut TextCache,
    layout: &Layout,
    hovered_card: Option<usize>,
    header: &HeaderText<'_>,
) -> Result<(), cairo::Error> {
    // The dim *replaces* whatever is there (SOURCE), never blends onto it. In
    // the C bar a reused buffer still held its previous frame and every
    // hover blended another 30% layer on (BUG.md, "Background got darker on
    // hover"). Here each paint starts from a fresh image, so it can't happen,
    // but SOURCE states the intent.
    cr.save()?;
    cr.set_operator(cairo::Operator::Source);
    cr.set_source_rgba(COLORS.crust.r, COLORS.crust.g, COLORS.crust.b, DIM_ALPHA);
    cr.paint()?;
    cr.restore()?;

    draw_rect(
        cr,
        layout.panel,
        COLORS.mantle,
        COLORS.sapphire,
        BAR.module_border_width,
    )?;
    let summary = format!("  \u{b7}  {}", header.identity);
    let header_row = Header {
        icon: Some(ICON_POWER),
        icon_color: COLORS.sapphire,
        title: "Power",
        summary: &summary,
        pill: header.uptime,
        pill_fill: COLORS.sapphire,
    };
    let origin = (
        layout.panel.x + OUTER_MARGIN,
        layout.panel.y + OUTER_MARGIN,
        CONTENT_W,
    );
    popup_chrome::draw_header(cr, text, origin, &header_row)?;

    for (index, (card, rect)) in CARDS.iter().zip(layout.cards).enumerate() {
        draw_card(cr, text, card, rect, hovered_card == Some(index))?;
    }
    Ok(())
}

/// One card: icon, name, description, stacked and centered.
fn draw_card(
    cr: &cairo::Context,
    text: &mut TextCache,
    card: &Card,
    rect: BoxRect,
    hovered: bool,
) -> Result<(), cairo::Error> {
    // A hovered card is both "current" (accent border) and lightened.
    popup_chrome::draw_card(cr, rect, hovered, hovered, card.accent)?;
    let icon = card.icon.to_string();
    let (_, icon_h) = text.size(&icon, FONT.display, false);
    let (_, label_h) = text.size(card.label, FONT.label, true);
    let (_, detail_h) = text.size(card.detail, FONT.xs, false);
    let total = icon_h + ICON_LABEL_GAP + label_h + LABEL_DETAIL_GAP + detail_h;
    let mut y = rect.y + (rect.h - total) / 2.0;
    let row = |top: f64, h: f64| BoxRect {
        x: rect.x,
        y: top,
        w: rect.w,
        h,
    };
    text.draw_centered(cr, row(y, icon_h), &icon, FONT.display, false, card.accent)?;
    y += icon_h + ICON_LABEL_GAP;
    let name_color = if hovered { card.accent } else { COLORS.text };
    text.draw_centered(
        cr,
        row(y, label_h),
        card.label,
        FONT.label,
        true,
        name_color,
    )?;
    y += label_h + LABEL_DETAIL_GAP;
    text.draw_centered(
        cr,
        row(y, detail_h),
        card.detail,
        FONT.xs,
        false,
        COLORS.overlay0,
    )
}

/// "user@host" for the header.
fn identity() -> String {
    let user = std::env::var("USER").unwrap_or_else(|_| "?".to_owned());
    let host = std::fs::read_to_string("/proc/sys/kernel/hostname").unwrap_or_default();
    format!("{user}@{}", host.trim())
}

/// The machine's uptime for the header pill, or `None` if `/proc/uptime`
/// can't be read (then there's no pill).
fn uptime() -> Option<String> {
    let text = std::fs::read_to_string("/proc/uptime").ok()?;
    let seconds: f64 = text.split_whitespace().next()?.parse().ok()?;
    Some(format_uptime(seconds))
}

/// "up 2d 3h", "up 3h 4m" or "up 5m": the two largest units, as in C.
fn format_uptime(seconds: f64) -> String {
    let minutes = whole_px((seconds / 60.0).floor());
    let (hours, days) = (minutes / 60, minutes / 60 / 24);
    if days > 0 {
        format!("up {days}d {}h", hours % 24)
    } else if hours > 0 {
        format!("up {hours}h {}m", minutes % 60)
    } else {
        format!("up {minutes}m")
    }
}

#[cfg(test)]
mod tests {
    use super::{Layout, format_uptime};

    #[test]
    fn uptime_shows_the_two_largest_units() {
        assert_eq!(format_uptime(59.0), "up 0m");
        assert_eq!(format_uptime(5.0 * 60.0 + 30.0), "up 5m");
        assert_eq!(format_uptime((3.0 * 60.0 + 4.0) * 60.0), "up 3h 4m");
        assert_eq!(
            format_uptime(((2.0 * 24.0 + 3.0) * 60.0 + 59.0) * 60.0),
            "up 2d 3h"
        );
    }

    #[test]
    fn panel_is_centered_on_the_surface() {
        let layout = Layout::for_size((1920, 1040));
        assert_eq!((layout.panel.x, layout.panel.y), (766.0, 436.0));
        assert_eq!(
            layout.card_at(layout.panel.x + 30.0, layout.panel.y + 100.0),
            Some(0)
        );
        assert_eq!(
            layout.card_at(layout.panel.x + 30.0, layout.panel.y + 20.0),
            None
        ); // header
    }
}
