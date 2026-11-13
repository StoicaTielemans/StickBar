//! The media dropdown (the C bar's `modules/media/media_popup.c`;
//! `docs/modules/media.md` describes each part): every player, not just the
//! one the bar shows.
//!
//! - **Header:** `Media · N players` and a pill for the liveliest state
//!   (`playing` green, then `paused` yellow, then `stopped`).
//! - **One card per player**, the bar's player with the accent border: the
//!   app name in caps and its status word, the title, `artist · album` (left
//!   out when both are empty), a timeline (`pos / length`; a click seeks
//!   there if the player can seek), and prev / play-pause / next chips.
//!
//! - **The DAC EQ card** to the left of the player cards, side by side under
//!   the header (`eq_section.rs`): its header and pill, the curve, and one
//!   chip per profile.
//!
//! The columns' widths are fixed, so a longer title never resizes it; the
//! player count and the artist lines change the height (the taller column
//! wins), which recreates the surface. With no player it's just the header
//! and the EQ column, one column wide.

use std::fs::File;

use smithay_client_toolkit::reexports::client::{
    QueueHandle,
    protocol::{wl_output::WlOutput, wl_surface::WlSurface},
};

use super::{
    Media, display_title,
    eq_section::{self, EqSection},
    graphs::{self, Graphs, VIS_ROW_GAP},
};
use crate::{
    bar::{
        popup_host::PopupHost,
        popup_surface::{PopupEnv, PopupSurface, render_image},
        strip_surface::StripEnv,
    },
    config::{
        COLORS, Color, FONT,
        glyphs::{ICON_MEDIA_NEXT, ICON_MEDIA_PAUSE, ICON_MEDIA_PLAY, ICON_MEDIA_PREV},
    },
    mpris::{MediaCommand, PlayerInfo},
    render::{
        BoxRect, meter_bar,
        module_box::draw_rect,
        popup_chrome::{self, Header},
        popup_style::{CARD_GAP, COL_GAP, HEADER_H, OUTER_MARGIN, SECTION_GAP},
        text::TextCache,
        whole_px,
    },
    visualizer::MAX_SOURCES,
    wayland::App,
};

// Layout, local to this popup (CLAUDE.md "Conventions").
/// The player column's width (fixed: titles truncate instead).
const CONTENT_W: f64 = 360.0;
/// The EQ column's width, left of the players (its five chips fit in one row).
const EQ_COL_W: f64 = 360.0;
/// Between the EQ column and the player column.
const COLUMN_GAP: f64 = SECTION_GAP;
/// The player column's left edge.
const CARDS_X: f64 = OUTER_MARGIN + EQ_COL_W + COLUMN_GAP;
/// Card edge to its content.
const CARD_PAD: f64 = 10.0;
/// The app name / status line.
const TOP_LINE_H: f64 = 14.0;
/// The title line.
const TITLE_H: f64 = 20.0;
/// The artist line.
const ARTIST_H: f64 = 16.0;
/// Above the timeline.
const TIMELINE_GAP: f64 = 6.0;
/// The timeline row (meter + time text): also its click area.
const TIMELINE_ROW_H: f64 = 14.0;
/// The meter's own height.
const TIMELINE_METER_H: f64 = 6.0;
/// The meter's segments.
const TIMELINE_SEGMENTS: u32 = 36;
/// Timeline to the chips.
const CONTROLS_GAP: f64 = 8.0;
/// A chip's width.
const BUTTON_W: f64 = 38.0;
/// A chip's height.
const BUTTON_H: f64 = 28.0;
/// Between chips.
const BUTTON_GAP: f64 = 6.0;
/// The chips' glyph size: larger than the text, so they read as buttons.
const CONTROL_ICON_PX: i32 = 22;
/// Lengths past a day are bogus (livestreams report sentinels), as the
/// reference's `maxSaneDuration`.
const MAX_SANE_DURATION_US: i64 = 86_400 * 1_000_000;
/// The dropdown's accent: the media box's colour.
const ACCENT: Color = COLORS.yellow;
/// The widest the time text gets (the font is monospace): the meter keeps a
/// fixed width as the seconds tick.
const WIDEST_TIME: &str = "00:00:00 / 00:00:00";

/// A control chip.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Button {
    /// Previous track.
    Prev,
    /// Play/pause.
    PlayPause,
    /// Next track.
    Next,
}

impl Button {
    /// Every chip, left to right.
    const ALL: [Self; 3] = [Self::Prev, Self::PlayPause, Self::Next];

    /// Its left edge.
    fn x(self) -> f64 {
        let slot = match self {
            Self::Prev => 0.0,
            Self::PlayPause => 1.0,
            Self::Next => 2.0,
        };
        INNER_X + slot * (BUTTON_W + BUTTON_GAP)
    }

    /// Whether it does anything for `player`.
    fn enabled(self, player: &PlayerInfo) -> bool {
        match self {
            Self::Prev => player.can_prev,
            Self::PlayPause => true,
            Self::Next => player.can_next,
        }
    }

    /// What a click on it sends for player `id`.
    const fn command(self, player: u64) -> MediaCommand {
        match self {
            Self::Prev => MediaCommand::Previous { player },
            Self::PlayPause => MediaCommand::PlayPause { player },
            Self::Next => MediaCommand::Next { player },
        }
    }
}

/// What a press on the dropdown asks for.
#[derive(Debug)]
pub(crate) enum MediaPress {
    /// A player chip or timeline: send this to the player.
    Player(MediaCommand),
    /// An EQ chip: apply this profile.
    EqProfile(&'static str),
}

/// A graph panel's (x, width) in the popup (the tests' `dump visualizer`).
pub(crate) const fn graph_panel() -> (f64, f64) {
    (INNER_X, INNER_W)
}

/// The player column's content left edge.
const INNER_X: f64 = CARDS_X + CARD_PAD;
/// The card's content width.
const INNER_W: f64 = CONTENT_W - CARD_PAD * 2.0;

/// One card's geometry, popup-local.
#[derive(Clone, Copy, Debug)]
struct Card {
    /// Whose card.
    player: u64,
    /// Top.
    y: f64,
    /// Height.
    h: f64,
    /// The artist/album line is shown.
    has_artist: bool,
    /// The spectrum graph panel's top, for the first cards while the
    /// visualizer is available.
    vis_y: Option<f64>,
    /// The timeline row's top.
    timeline_y: f64,
    /// The chips' top.
    buttons_y: f64,
}

/// What an open dropdown shows.
struct Content {
    /// The cards, one per player.
    cards: Vec<Card>,
    /// The popup's size.
    size: (f64, f64),
    /// The chip under the pointer (card index, chip).
    hovered: Option<(usize, Button)>,
    /// The card whose (seekable) timeline is under the pointer.
    hovered_timeline: Option<usize>,
    /// The timeline meter's width, measured once at open (it's text-based,
    /// and hit-tests run where no text cache is at hand).
    meter_w: f64,
    /// The EQ card, below the player cards.
    eq: EqSection,
    /// The EQ chip under the pointer.
    hovered_chip: Option<usize>,
}

/// An open dropdown.
struct Open {
    /// The cards' spectrum graphs. Declared before `host`, so their strip
    /// subsurfaces are destroyed before the surface they sit on.
    graphs: Graphs,
    /// Its surfaces.
    host: PopupHost,
    /// What it shows, kept apart from `host` so a paint can borrow both.
    content: Content,
}

/// The media dropdown: closed or open.
#[derive(Default)]
pub(crate) struct MediaPopup {
    /// The open dropdown, if any.
    open: Option<Open>,
}

impl MediaPopup {
    /// Opens the dropdown under the box `trigger` on `output`, replacing an
    /// open one (with or without players: the EQ card is always there).
    pub(crate) fn open<'o>(
        &mut self,
        (env, strips): (&PopupEnv<'_>, Option<&StripEnv<'_>>),
        (output, trigger): (&WlOutput, BoxRect),
        all_outputs: impl Iterator<Item = &'o WlOutput>,
        media: &Media,
        text: &mut TextCache,
    ) {
        self.open = None;
        let mut eq = EqSection::new(text, (OUTER_MARGIN, EQ_COL_W));
        let (cards, size) = layout(media, &mut eq);
        let host = PopupHost::open_centered(
            env,
            output,
            trigger,
            (whole_px(size.0), whole_px(size.1)),
            all_outputs,
        );
        self.open = Some(Open {
            graphs: Graphs::default(),
            host,
            content: Content {
                cards,
                size,
                hovered: None,
                hovered_timeline: None,
                meter_w: timeline_meter_w(text),
                eq,
                hovered_chip: None,
            },
        });
        self.update_graphs(strips, media);
        self.paint(media, text);
    }

    /// The players changed: re-lays out, and recreates the surface if the
    /// height changed (resizing a layer surface in place made Hyprland unmap
    /// it on shrink, as with the mixer). It stays open with no player left:
    /// the EQ card is still there.
    pub(crate) fn refresh(
        &mut self,
        (env, strips): (&PopupEnv<'_>, Option<&StripEnv<'_>>),
        media: &Media,
        text: &mut TextCache,
    ) {
        let Some(open) = self.open.as_mut() else {
            return;
        };
        let content = &mut open.content;
        let (cards, size) = layout(media, &mut content.eq);
        if content.hovered.is_some_and(|(card, _)| card >= cards.len()) {
            content.hovered = None;
        }
        if content
            .hovered_timeline
            .is_some_and(|card| card >= cards.len())
        {
            content.hovered_timeline = None;
        }
        content.cards = cards;
        // Height for players and artist lines; width for none <-> some.
        let px = |(w, h): (f64, f64)| (whole_px(w), whole_px(h));
        if px(size) != px(content.size) {
            open.host.resize(env, (whole_px(size.0), whole_px(size.1)));
        }
        content.size = size;
        self.update_graphs(strips, media);
        self.paint(media, text);
    }

    /// Places and draws the graphs for the current layout (on the current
    /// menu surface: a resize made a new one). Runs before the paint whose
    /// commit applies the strips' positions.
    fn update_graphs(&mut self, strips: Option<&StripEnv<'_>>, media: &Media) {
        let Some(open) = self.open.as_mut() else {
            return;
        };
        let parent = open.host.menu().wl_surface().clone();
        let rows: Vec<Option<f64>> = open
            .content
            .cards
            .iter()
            .take(MAX_SOURCES)
            .map(|c| c.vis_y)
            .collect();
        open.graphs
            .update(strips, &parent, (INNER_X, INNER_W), &rows, media);
    }

    /// (shown, ready) graph masks for the visualizer's pump ((0, 0) closed).
    pub(crate) fn graph_masks(&self) -> (u32, u32) {
        self.open.as_ref().map_or((0, 0), |o| o.graphs.masks())
    }

    /// Redraws the graphs of the slots in `changed` (a visualizer frame).
    pub(crate) fn draw_graphs(&mut self, changed: u32, qh: &QueueHandle<App>, media: &Media) {
        if let Some(open) = self.open.as_mut() {
            open.graphs.draw_changed(changed, qh, media);
        }
    }

    /// A frame callback, if `surface` is one of the graphs' strips. Returns
    /// whether it was.
    pub(crate) fn strip_frame_done(&mut self, surface: &WlSurface, qh: &QueueHandle<App>) -> bool {
        self.open
            .as_mut()
            .is_some_and(|o| o.graphs.frame_done(surface, qh))
    }

    /// Which slots should capture now, and whose process: a graph on screen
    /// whose player is playing (and, in tests, is one of their fakes). All
    /// off when closed.
    pub(crate) fn slot_wants(&self, media: &Media) -> [(bool, u32); MAX_SOURCES] {
        let mut wants = [(false, 0); MAX_SOURCES];
        let Some(open) = self.open.as_ref() else {
            return wants;
        };
        for ((slot, want), card) in wants.iter_mut().enumerate().zip(&open.content.cards) {
            let Some(player) = media.player(card.player) else {
                continue;
            };
            let active = card.vis_y.is_some()
                && open.graphs.shown(slot)
                && player.is_playing()
                && media.visualizer.capture_allowed(&player.app);
            *want = (active, player.pid);
        }
        wants
    }

    /// Each graph-carrying card's (app, slot, panel top), for the tests'
    /// `dump visualizer`.
    pub(crate) fn test_graphs<'m>(&self, media: &'m Media) -> Vec<(&'m str, usize, f64)> {
        let Some(open) = self.open.as_ref() else {
            return Vec::new();
        };
        open.content
            .cards
            .iter()
            .enumerate()
            .filter_map(|(slot, card)| {
                let app = media.player(card.player)?.app.as_str();
                card.vis_y.map(|y| (app, slot, y))
            })
            .collect()
    }

    /// Closes the dropdown (no-op if closed).
    pub(crate) fn close(&mut self) {
        self.open = None;
    }

    /// Whether it's open.
    pub(crate) const fn is_open(&self) -> bool {
        self.open.is_some()
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

    /// How many cards are shown (the tests' `menu`), `None` when closed.
    pub(crate) fn card_count(&self) -> Option<usize> {
        self.open.as_ref().map(|o| o.content.cards.len())
    }

    /// How many EQ profile chips are shown (the tests' `menu`), `None` when closed.
    pub(crate) fn eq_chip_count(&self) -> Option<usize> {
        self.open.as_ref().map(|_| eq_section::PROFILES.len())
    }

    /// Paints the open dropdown.
    pub(crate) fn paint(&mut self, media: &Media, text: &mut TextCache) {
        if let Some(Open {
            host,
            content,
            graphs,
        }) = self.open.as_mut()
        {
            host.menu_mut()
                .paint(|cr| draw(cr, text, media, (content, graphs)));
        }
    }

    /// The pointer moved to (x, y) on the dropdown: the hover follows.
    pub(crate) fn motion(&mut self, (x, y): (f64, f64), media: &Media, text: &mut TextCache) {
        let Some(open) = self.open.as_mut() else {
            return;
        };
        let content = &mut open.content;
        let hovered = button_at(content, x, y);
        let hovered_timeline = timeline_at(content, media, x, y).map(|(card, _)| card);
        let hovered_chip = content.eq.chip_at(x, y);
        let now = (hovered, hovered_timeline, hovered_chip);
        if now
            != (
                content.hovered,
                content.hovered_timeline,
                content.hovered_chip,
            )
        {
            content.hovered = hovered;
            content.hovered_timeline = hovered_timeline;
            content.hovered_chip = hovered_chip;
            self.paint(media, text);
        }
    }

    /// A press at (x, y) on the dropdown (any button, as in C): what it asks
    /// for, if it hit a seekable timeline, a player chip, or an EQ chip while
    /// no profile switch is under way. The dropdown stays open.
    pub(crate) fn press(&self, (x, y): (f64, f64), media: &Media) -> Option<MediaPress> {
        let content = &self.open.as_ref()?.content;
        if let Some(chip) = content.eq.chip_at(x, y) {
            if media.eq.busy() {
                return None;
            }
            return eq_section::PROFILES
                .get(chip)
                .map(|name| MediaPress::EqProfile(name));
        }
        if let Some((card, fraction)) = timeline_at(content, media, x, y) {
            let player = media.player(content.cards.get(card)?.player)?;
            // The length is at most a day, so this is exact enough in f64.
            let position_us = fraction * micros_f64(player.length_us);
            return Some(MediaPress::Player(MediaCommand::Seek {
                player: player.id,
                position_us: micros_i64(position_us),
            }));
        }
        let (card, button) = button_at(content, x, y)?;
        Some(MediaPress::Player(
            button.command(content.cards.get(card)?.player),
        ))
    }

    /// Whether a click at (x, y) would do something (the hand cursor): an
    /// enabled chip, a seekable timeline, or an EQ chip while no profile
    /// switch is under way.
    pub(crate) fn over_clickable(&self, (x, y): (f64, f64), media: &Media) -> bool {
        let Some(open) = self.open.as_ref() else {
            return false;
        };
        let content = &open.content;
        let on_chip = button_at(content, x, y).is_some_and(|(card, button)| {
            content
                .cards
                .get(card)
                .and_then(|c| media.player(c.player))
                .is_some_and(|p| button.enabled(p))
        });
        let on_eq_chip = !media.eq.busy() && content.eq.chip_at(x, y).is_some();
        on_chip || on_eq_chip || timeline_at(content, media, x, y).is_some()
    }

    /// Where the tests' `target eq <what>` points, popup-local
    /// ([`EqSection::target`]).
    pub(crate) fn eq_target(&self, what: &str) -> Option<(f64, f64)> {
        self.open.as_ref()?.content.eq.target(what)
    }

    /// Where `what` (`prev`, `next`, `play` or `timeline:FRACTION`) is on the
    /// card of the player called `app`, popup-local (the tests' `target media`).
    pub(crate) fn target(&self, (app, what): (&str, &str), media: &Media) -> Option<(f64, f64)> {
        let content = &self.open.as_ref()?.content;
        let card = content
            .cards
            .iter()
            .find(|c| media.player(c.player).is_some_and(|p| p.app == app))?;
        if let Some(fraction) = what.strip_prefix("timeline:") {
            let fraction = fraction.parse::<f64>().ok()?;
            return Some((
                INNER_X + fraction * content.meter_w,
                card.timeline_y + TIMELINE_ROW_H / 2.0,
            ));
        }
        let button = match what {
            "prev" => Button::Prev,
            "next" => Button::Next,
            _ => Button::PlayPause,
        };
        Some((button.x() + BUTTON_W / 2.0, card.buttons_y + BUTTON_H / 2.0))
    }

    /// Writes what the open dropdown shows, drawn again, to a PNG (the tests'
    /// `snapshot`).
    pub(crate) fn snapshot(
        &self,
        path: &str,
        media: &Media,
        text: &mut TextCache,
    ) -> Result<(), String> {
        let open = self.open.as_ref().ok_or("no open menu to snapshot")?;
        let (width, height) = open.host.menu().menu_size().ok_or("not a menu")?;
        let image = render_image(width, height, |cr| {
            draw(cr, text, media, (&open.content, &open.graphs))
        })
        .map_err(|e| e.to_string())?;
        let mut file = File::create(path).map_err(|e| e.to_string())?;
        image.write_to_png(&mut file).map_err(|e| e.to_string())
    }
}

/// µs as f64. Exact below 2^53 µs (285 years), so for every sane length.
fn micros_f64(us: i64) -> f64 {
    // i32 halves keep the conversion lossless without an `as` cast.
    let high = i32::try_from(us >> 32).unwrap_or(0);
    let low = u32::try_from(us & 0xffff_ffff).unwrap_or(0);
    f64::from(high) * 4_294_967_296.0 + f64::from(low)
}

/// A non-negative µs value as i64, rounded down: whole seconds and the
/// microseconds left over, each small enough for [`whole_px`] (the crate's one
/// f64-to-int conversion), so no float cast is needed here.
fn micros_i64(us: f64) -> i64 {
    let secs = (us / 1e6).floor().max(0.0);
    let rest = (us - secs * 1e6).max(0.0);
    i64::from(whole_px(secs)) * 1_000_000 + i64::from(whole_px(rest))
}

/// A length the timeline can show and seek within: reported, and not absurd.
const fn has_sane_length(player: &PlayerInfo) -> bool {
    player.length_us > 0 && player.length_us < MAX_SANE_DURATION_US
}

/// `artist · album`, either alone, or "".
fn artist_text(player: &PlayerInfo) -> String {
    match (player.artist.is_empty(), player.album.is_empty()) {
        (false, false) => format!("{} \u{b7} {}", player.artist, player.album),
        (false, true) => player.artist.clone(),
        (true, _) => player.album.clone(),
    }
}

/// `m:ss`, or `h:mm:ss` from an hour up.
fn format_time(us: i64) -> String {
    let total = us.max(0) / 1_000_000;
    let (h, m, s) = (total / 3600, (total % 3600) / 60, total % 60);
    if h > 0 {
        format!("{h}:{m:02}:{s:02}")
    } else {
        format!("{m}:{s:02}")
    }
}

/// The status word and its colour ("" counts as stopped).
fn status_word(player: &PlayerInfo) -> (&'static str, Color) {
    match player.status.as_str() {
        "Playing" => ("playing", COLORS.green),
        "Paused" => ("paused", COLORS.yellow),
        _ => ("stopped", COLORS.overlay0),
    }
}

/// The cards for `media`'s players (right column), the EQ card placed left
/// of them, and the popup's size: two columns wide with players, just the EQ
/// column without, and as tall as the taller column.
fn layout(media: &Media, eq: &mut EqSection) -> (Vec<Card>, (f64, f64)) {
    let top = OUTER_MARGIN + HEADER_H + SECTION_GAP;
    let mut y = top;
    let count = media.players.len();
    let mut cards = Vec::with_capacity(count);
    for (i, player) in media.players.iter().enumerate() {
        let has_artist = !artist_text(player).is_empty();
        let mut h = CARD_PAD + TOP_LINE_H + TITLE_H;
        if has_artist {
            h += ARTIST_H;
        }
        // The graph row (every card in reach of a slot gets it, playing or
        // not, so play/pause never changes the height).
        let vis_y =
            (media.visualizer.available() && i < MAX_SOURCES).then_some(y + h + VIS_ROW_GAP);
        if vis_y.is_some() {
            h += VIS_ROW_GAP + graphs::row_h();
        }
        let timeline_y = y + h + TIMELINE_GAP;
        h += TIMELINE_GAP + TIMELINE_ROW_H + CONTROLS_GAP;
        let buttons_y = y + h;
        h += BUTTON_H + CARD_PAD;
        cards.push(Card {
            player: player.id,
            y,
            h,
            has_artist,
            vis_y,
            timeline_y,
            buttons_y,
        });
        y += h + if i + 1 < count { CARD_GAP } else { 0.0 };
    }
    eq.place(top);
    let bottom = y.max(top + eq.height());
    let width = if count == 0 {
        EQ_COL_W + OUTER_MARGIN * 2.0
    } else {
        CARDS_X + CONTENT_W + OUTER_MARGIN
    };
    (cards, (width, bottom + OUTER_MARGIN))
}

/// The timeline meter's width: the row minus the widest time text.
fn timeline_meter_w(text: &mut TextCache) -> f64 {
    INNER_W - text.width(WIDEST_TIME, FONT.sm, false) - COL_GAP
}

/// The chip under (x, y).
fn button_at(content: &Content, x: f64, y: f64) -> Option<(usize, Button)> {
    content.cards.iter().enumerate().find_map(|(i, card)| {
        if !(card.buttons_y..card.buttons_y + BUTTON_H).contains(&y) {
            return None;
        }
        Button::ALL
            .into_iter()
            .find(|b| (b.x()..b.x() + BUTTON_W).contains(&x))
            .map(|b| (i, b))
    })
}

/// The card whose timeline is under (x, y), and how far along (0..1); only
/// for a player that can seek and has a sane length. The hit area is the
/// meter's width over the whole 14 px row, kinder than the 6 px meter.
fn timeline_at(content: &Content, media: &Media, x: f64, y: f64) -> Option<(usize, f64)> {
    let meter_w = content.meter_w;
    if !(INNER_X..INNER_X + meter_w).contains(&x) {
        return None;
    }
    let (i, card) = content
        .cards
        .iter()
        .enumerate()
        .find(|(_, c)| (c.timeline_y..c.timeline_y + TIMELINE_ROW_H).contains(&y))?;
    let player = media.player(card.player)?;
    (player.can_seek && has_sane_length(player)).then(|| (i, (x - INNER_X) / meter_w))
}

/// The whole dropdown.
fn draw(
    cr: &cairo::Context,
    text: &mut TextCache,
    media: &Media,
    (content, graphs): (&Content, &Graphs),
) -> Result<(), cairo::Error> {
    popup_chrome::draw_frame(cr, content.size.0, content.size.1, ACCENT)?;
    draw_header(
        cr,
        text,
        media,
        (content.cards.len(), content.size.0 - OUTER_MARGIN * 2.0),
    )?;
    let active = media.active().map(|p| p.id);
    for (i, card) in content.cards.iter().enumerate() {
        if let Some(player) = media.player(card.player) {
            draw_card(
                cr,
                text,
                media,
                (i, card, player),
                active == Some(player.id),
                content,
            )?;
        }
        // The graph under its strip: hidden on screen, but in snapshots.
        if let Some(vis_y) = card.vis_y {
            graphs.paint_under(cr, i, vis_y, media)?;
        }
    }
    content.eq.draw(cr, text, &media.eq, content.hovered_chip)
}

/// The header row: the pill is the liveliest state across players.
fn draw_header(
    cr: &cairo::Context,
    text: &mut TextCache,
    media: &Media,
    (count, width): (usize, f64),
) -> Result<(), cairo::Error> {
    let any = |status: &str| media.players.iter().any(|p| p.status == status);
    let (pill, pill_fill) = if any("Playing") {
        ("playing", COLORS.green)
    } else if any("Paused") {
        ("paused", COLORS.yellow)
    } else {
        ("stopped", COLORS.overlay1)
    };
    let summary = match count {
        0 => "  \u{b7}  no players".to_owned(),
        1 => "  \u{b7}  1 player".to_owned(),
        _ => format!("  \u{b7}  {count} players"),
    };
    let header = Header {
        icon: Some(ICON_MEDIA_PLAY),
        icon_color: ACCENT,
        title: "Media",
        summary: &summary,
        pill: Some(pill),
        pill_fill,
    };
    popup_chrome::draw_header(cr, text, (OUTER_MARGIN, OUTER_MARGIN, width), &header)
}

/// One player's card; `active` (the bar's player) gets the accent border.
fn draw_card(
    cr: &cairo::Context,
    text: &mut TextCache,
    media: &Media,
    (i, card, player): (usize, &Card, &PlayerInfo),
    active: bool,
    content: &Content,
) -> Result<(), cairo::Error> {
    let rect = BoxRect {
        x: CARDS_X,
        y: card.y,
        w: CONTENT_W,
        h: card.h,
    };
    popup_chrome::draw_card(cr, rect, active, false, ACCENT)?;

    let mut y = card.y + CARD_PAD;
    // The app name in caps (ASCII only, as in C) and the status word.
    let (status, status_color) = status_word(player);
    let status_w = text.width(status, FONT.xs, true);
    let app = text.truncate(
        &player.app.to_ascii_uppercase(),
        INNER_W - status_w - COL_GAP,
        FONT.xs,
        true,
    );
    text.draw_left(
        cr,
        (INNER_X, y, TOP_LINE_H),
        &app,
        FONT.xs,
        true,
        COLORS.overlay0,
    )?;
    text.draw_right(
        cr,
        (INNER_X + INNER_W, y, TOP_LINE_H),
        status,
        FONT.xs,
        true,
        status_color,
    )?;
    y += TOP_LINE_H;

    let title = text.truncate(display_title(player), INNER_W, FONT.label, true);
    text.draw_left(
        cr,
        (INNER_X, y, TITLE_H),
        &title,
        FONT.label,
        true,
        COLORS.text,
    )?;
    y += TITLE_H;

    if card.has_artist {
        let artist = text.truncate(&artist_text(player), INNER_W, FONT.md, false);
        text.draw_left(
            cr,
            (INNER_X, y, ARTIST_H),
            &artist,
            FONT.md,
            false,
            COLORS.subtext0,
        )?;
    }

    let hovered_timeline = content.hovered_timeline == Some(i);
    let timeline = (card.timeline_y, content.meter_w);
    draw_timeline(
        cr,
        text,
        (player, media.position(player.id)),
        timeline,
        hovered_timeline,
    )?;
    for button in Button::ALL {
        let hovered = content.hovered == Some((i, button));
        draw_button(cr, text, player, (button, card.buttons_y), hovered)?;
    }
    Ok(())
}

/// The timeline: a meter and `pos / length` (just the elapsed time without a
/// sane length, with the meter empty). `hovered` (only when seekable)
/// lightens the empty part, the chips' "clickable" cue.
fn draw_timeline(
    cr: &cairo::Context,
    text: &mut TextCache,
    (player, position_us): (&PlayerInfo, i64),
    (y, meter_w): (f64, f64),
    hovered: bool,
) -> Result<(), cairo::Error> {
    let sane = has_sane_length(player);
    let mut pos = position_us.max(0);
    if sane {
        pos = pos.min(player.length_us);
    }
    let label = if sane {
        format!("{} / {}", format_time(pos), format_time(player.length_us))
    } else {
        format_time(pos)
    };
    let fraction = if sane {
        micros_f64(pos) / micros_f64(player.length_us)
    } else {
        0.0
    };
    let meter = BoxRect {
        x: INNER_X,
        y: y + (TIMELINE_ROW_H - TIMELINE_METER_H) / 2.0,
        w: meter_w,
        h: TIMELINE_METER_H,
    };
    let empty = if hovered {
        COLORS.surface1
    } else {
        COLORS.surface0
    };
    meter_bar::draw(
        cr,
        meter,
        fraction,
        TIMELINE_SEGMENTS,
        &[ACCENT, ACCENT, COLORS.peach],
        empty,
    )?;
    text.draw_right(
        cr,
        (INNER_X + INNER_W, y, TIMELINE_ROW_H),
        &label,
        FONT.sm,
        false,
        COLORS.subtext0,
    )
}

/// One chip. A disabled one is dimmed and never lights up.
fn draw_button(
    cr: &cairo::Context,
    text: &mut TextCache,
    player: &PlayerInfo,
    (button, y): (Button, f64),
    hovered: bool,
) -> Result<(), cairo::Error> {
    let enabled = button.enabled(player);
    let glyph = match button {
        Button::Prev => ICON_MEDIA_PREV,
        Button::Next => ICON_MEDIA_NEXT,
        Button::PlayPause if player.is_playing() => ICON_MEDIA_PAUSE,
        Button::PlayPause => ICON_MEDIA_PLAY,
    };
    let rect = BoxRect {
        x: button.x(),
        y,
        w: BUTTON_W,
        h: BUTTON_H,
    };
    let fill = if hovered && enabled {
        COLORS.surface1
    } else {
        COLORS.surface0
    };
    draw_rect(cr, rect, fill, COLORS.mantle, 0.0)?;
    let color = if enabled { ACCENT } else { COLORS.overlay0 };
    text.draw_centered(cr, rect, &glyph.to_string(), CONTROL_ICON_PX, false, color)
}

#[cfg(test)]
mod tests {
    use super::{format_time, micros_f64, micros_i64};

    #[test]
    fn times() {
        assert_eq!(format_time(0), "0:00");
        assert_eq!(format_time(65_400_000), "1:05");
        assert_eq!(format_time(3_725_000_000), "1:02:05");
        assert_eq!(format_time(-5), "0:00");
    }

    #[test]
    fn microsecond_conversions() {
        assert!((micros_f64(200_000_000) - 2e8).abs() < 1e-6);
        assert!((micros_f64(86_400_000_000) - 8.64e10).abs() < 1e-3);
        assert_eq!(micros_i64(50_000_000.7), 50_000_000);
        assert_eq!(micros_i64(-3.0), 0);
        assert_eq!(micros_i64(8.64e10), 86_400_000_000);
    }
}
