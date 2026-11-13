//! The media box (the C bar's `modules/media/media.c`): the active player as
//! `<play/pause icon>  artist - title` in yellow, **centred in the bar** (the
//! one module in neither side region, like the reference's `centerLayout`).
//! It's drawn after both side regions and clamped between them, so a long
//! title truncates instead of covering its neighbours (BUG.md "Media"); with
//! no room for even the icon and `...`, it takes no space.
//! With no player it stays, at the user's request: `▶  No media playing` until
//! something has played since the bar started, then `▶  <the last track>`
//! (kept in memory only). That also keeps the dropdown, and so the EQ card,
//! one right-click away when nothing plays.
//! A left click is `PlayPause` on the shown player (nothing without one); a
//! right click opens the dropdown
//! of every player, plus the DAC EQ's card (`popup.rs`, `eq_section.rs`). The
//! EQ has nothing in the bar itself, at the user's request.

pub(crate) mod eq_section;
pub(crate) mod graphs;
pub(crate) mod popup;

use std::collections::HashMap;

use crate::{
    config::{
        BAR, COLORS, FONT,
        glyphs::{ICON_MEDIA_PAUSE, ICON_MEDIA_PLAY},
    },
    eq::EqState,
    mpris::PlayerInfo,
    render::{
        BoxRect,
        module_box::{self, Anchor},
        text::TextCache,
    },
    visualizer::Visualizer,
};

/// The track text's widest, as the reference's `Math.min(implicitWidth, 300)`.
const TRACK_TEXT_MAX_W: f64 = 300.0;
/// The idle box's text before anything has played since the bar started.
pub(crate) const NO_MEDIA_TEXT: &str = "No media playing";

/// The media players, as the `dbus` thread last reported them, and the DAC EQ.
#[derive(Debug, Default)]
pub(crate) struct Media {
    /// The visible players, in order.
    pub(crate) players: Vec<PlayerInfo>,
    /// Positions (µs) by player id: polled only while the dropdown is open,
    /// since players don't push them.
    positions: HashMap<u64, i64>,
    /// The dropdown's spectrum graphs' audio and levels.
    pub(crate) visualizer: Visualizer,
    /// The DAC EQ's state file, and the profile switch in progress.
    pub(crate) eq: EqState,
    /// `artist - title` of the last player the box showed, for the idle box
    /// once every player has gone. `None` until something has played.
    last_track: Option<String>,
}

impl Media {
    /// No players yet; the EQ's current state read from its file.
    pub(crate) fn new() -> Self {
        Self {
            eq: EqState::new(),
            ..Self::default()
        }
    }

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

    /// New player list: positions of players gone are dropped, and the
    /// shown player's track is remembered for the idle box.
    pub(crate) fn set_players(&mut self, players: Vec<PlayerInfo>) {
        self.positions
            .retain(|id, _| players.iter().any(|p| p.id == *id));
        self.players = players;
        if let Some(track) = self.active().map(track_text) {
            self.last_track = Some(track);
        }
    }

    /// What the idle box (no player) shows: the last track, or
    /// [`NO_MEDIA_TEXT`] before anything has played.
    pub(crate) fn idle_text(&self) -> &str {
        self.last_track.as_deref().unwrap_or(NO_MEDIA_TEXT)
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
/// starts), spanning `top..top + height`. `None` when it doesn't fit.
pub(crate) fn render(
    cr: &cairo::Context,
    text: &mut TextCache,
    media: &Media,
    bar_w: f64,
    (left_end, right_start): (f64, f64),
    (top, height): (f64, f64),
) -> Result<Option<BoxRect>, cairo::Error> {
    // A player: its state and track. None: the play icon and the idle text.
    let (icon, track) = match media.active() {
        Some(player) if player.is_playing() => (ICON_MEDIA_PAUSE, track_text(player)),
        Some(player) => (ICON_MEDIA_PLAY, track_text(player)),
        None => (ICON_MEDIA_PLAY, media.idle_text().to_owned()),
    };
    // The box stays centred, so it can only be twice as wide as the nearer
    // side region leaves room for.
    let center = bar_w / 2.0;
    let max_box_w = 2.0 * (center - left_end).min(right_start - center);
    let prefix_w = text.width(&format!("{icon}  "), FONT.label, true);
    let max_text_w = (max_box_w - BAR.module_padding - prefix_w).min(TRACK_TEXT_MAX_W);
    if max_text_w < text.width("...", FONT.label, true) {
        return Ok(None); // not even "..." fits: collapse
    }
    let track = text.truncate(&track, max_text_w, FONT.label, true);
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

#[cfg(test)]
mod tests {
    use super::{Media, NO_MEDIA_TEXT};
    use crate::mpris::PlayerInfo;

    #[test]
    fn idle_text_is_the_last_track_once_something_played() {
        let mut media = Media::default();
        assert_eq!(media.idle_text(), NO_MEDIA_TEXT);
        let player = PlayerInfo {
            id: 1,
            title: "Song".to_owned(),
            artist: "Band".to_owned(),
            ..PlayerInfo::default()
        };
        media.set_players(vec![player]);
        media.set_players(Vec::new());
        assert_eq!(media.idle_text(), "Band - Song");
        // A player with no metadata still counts: its name stands in.
        let video = PlayerInfo {
            id: 2,
            identity: "Firefox".to_owned(),
            ..PlayerInfo::default()
        };
        media.set_players(vec![video]);
        media.set_players(Vec::new());
        assert_eq!(media.idle_text(), "Firefox");
    }
}
