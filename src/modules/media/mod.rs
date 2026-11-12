//! The media box (the C bar's `modules/media/media.c`): the active player as
//! `<play/pause icon>  artist - title` in yellow, **centred in the bar** (the
//! one module in neither side region, like the reference's `centerLayout`).
//! It's drawn after both side regions and clamped between them, so a long
//! title truncates instead of covering its neighbours (BUG.md "Media"), and
//! with no player, or no room for even the icon and `...`, it takes no space.
//! A left click is `PlayPause` on that player; a right click opens the dropdown
//! of every player (`popup.rs`).

pub(crate) mod popup;

use std::collections::HashMap;

use crate::{
    config::{
        BAR, COLORS, FONT,
        glyphs::{ICON_MEDIA_PAUSE, ICON_MEDIA_PLAY},
    },
    mpris::PlayerInfo,
    render::{
        BoxRect,
        module_box::{self, Anchor},
        text::TextCache,
    },
};

/// The track text's widest, as the reference's `Math.min(implicitWidth, 300)`.
const TRACK_TEXT_MAX_W: f64 = 300.0;

/// The media players, as the `dbus` thread last reported them.
#[derive(Debug, Default)]
pub(crate) struct Media {
    /// The visible players, in order.
    pub(crate) players: Vec<PlayerInfo>,
    /// Positions (µs) by player id: polled only while the dropdown is open,
    /// since players don't push them.
    positions: HashMap<u64, i64>,
}

impl Media {
    /// The player the box shows: the first one playing, else the first.
    pub(crate) fn active(&self) -> Option<&PlayerInfo> {
        self.players
            .iter()
            .find(|p| p.is_playing())
            .or_else(|| self.players.first())
    }

    /// Player `id`.
    pub(crate) fn player(&self, id: u64) -> Option<&PlayerInfo> {
        self.players.iter().find(|p| p.id == id)
    }

    /// Player `id`'s position in µs (0 until it's been polled).
    pub(crate) fn position(&self, id: u64) -> i64 {
        self.positions.get(&id).copied().unwrap_or(0)
    }

    /// Records a position (polled, or the target of a seek just sent).
    pub(crate) fn set_position(&mut self, id: u64, position_us: i64) {
        let _previous = self.positions.insert(id, position_us);
    }

    /// New player list: positions of players gone are dropped.
    pub(crate) fn set_players(&mut self, players: Vec<PlayerInfo>) {
        self.positions
            .retain(|id, _| players.iter().any(|p| p.id == *id));
        self.players = players;
    }
}

/// The title to show: the track's, else the player's own name, else "Unknown".
pub(crate) fn display_title(player: &PlayerInfo) -> &str {
    [player.title.as_str(), player.identity.as_str()]
        .into_iter()
        .find(|t| !t.is_empty())
        .unwrap_or("Unknown")
}

/// `artist - title` (just the title without an artist), as the reference's
/// `trackText()`.
fn track_text(player: &PlayerInfo) -> String {
    let title = display_title(player);
    if player.artist.is_empty() {
        title.to_owned()
    } else {
        format!("{} - {title}", player.artist)
    }
}

/// Draws the box centred in a `bar_w` wide bar, between `left_end` (where the
/// left region ends, its gap included) and `right_start` (where the right one
/// starts), spanning `top..top + height`. `None` when nothing is drawn.
pub(crate) fn render(
    cr: &cairo::Context,
    text: &mut TextCache,
    media: &Media,
    bar_w: f64,
    (left_end, right_start): (f64, f64),
    (top, height): (f64, f64),
) -> Result<Option<BoxRect>, cairo::Error> {
    let Some(player) = media.active() else {
        return Ok(None);
    };
    let icon = if player.is_playing() {
        ICON_MEDIA_PAUSE
    } else {
        ICON_MEDIA_PLAY
    };
    // The box stays centred, so it can only be twice as wide as the nearer
    // side region leaves room for.
    let center = bar_w / 2.0;
    let max_box_w = 2.0 * (center - left_end).min(right_start - center);
    let prefix_w = text.width(&format!("{icon}  "), FONT.label, true);
    let max_text_w = (max_box_w - BAR.module_padding - prefix_w).min(TRACK_TEXT_MAX_W);
    if max_text_w < text.width("...", FONT.label, true) {
        return Ok(None); // not even "..." fits: collapse, like no player
    }
    let track = text.truncate(&track_text(player), max_text_w, FONT.label, true);
    let label = format!("{icon}  {track}");
    let box_w = text.width(&label, FONT.label, true) + BAR.module_padding;
    module_box::label(
        cr,
        text,
        (Anchor::FromLeft, (bar_w - box_w) / 2.0),
        (top, height),
        &label,
        COLORS.yellow,
    )
    .map(Some)
}
