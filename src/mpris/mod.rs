//! Media players over MPRIS (the C bar's `mpris/mpris_client.c`;
//! `docs/modules/media.md` has the protocol details). This runs on the
//! `dbus` thread, sharing the tray's connection and loop (`src/dbus/`); the
//! main thread only gets [`MediaEvent`]s and sends [`MediaCommand`]s.
//!
//! - **Discovery:** `ListNames` at start, then `NameOwnerChanged` (an
//!   `org.mpris.MediaPlayer2.*` name gaining an owner is a new player, losing
//!   it is one gone). `playerctld` is skipped: it mirrors whatever's playing
//!   under a name of its own, so every player would show twice.
//! - **Updates:** a player's `PropertiesChanged` is applied from the signal
//!   itself; only names sent as "invalidated" (no value) need a `GetAll`.
//! - **One visible player per process** ([`is_shadowed`]): Spotify also owns a
//!   stale `chromium.instanceN` mirror of itself (BUG.md "Media").
//! - **What the main thread sees:** after every change the visible players'
//!   [`PlayerInfo`]s, sent only if that list differs from the last one sent.
//!   So players that send `PropertiesChanged` for every volume or rate tweak
//!   (browsers do) cost no redraw at all.
//! - **Position isn't pushed by players** (the spec says it's polled), so
//!   it's only read when the main thread asks: once a second while the
//!   dropdown is open, never otherwise.

use std::collections::HashMap;

use calloop::channel;
use zbus::{
    Connection, Message,
    message::Flags,
    zvariant::{ObjectPath, OwnedValue, Value},
};

use crate::dbus::{text_of, unwrap_variant};

/// Every MPRIS player's bus name starts with this.
pub(crate) const NAME_PREFIX: &str = "org.mpris.MediaPlayer2.";
/// Every player lives at this one object path (unlike tray items).
pub(crate) const OBJECT_PATH: &str = "/org/mpris/MediaPlayer2";
/// The root interface (`Identity`).
const ROOT_INTERFACE: &str = "org.mpris.MediaPlayer2";
/// The player interface (status, metadata, controls).
const PLAYER_INTERFACE: &str = "org.mpris.MediaPlayer2.Player";
/// The properties interface.
const PROPERTIES: &str = "org.freedesktop.DBus.Properties";
/// Browser engines' per-process names contain this
/// (`...chromium.instance57585`, `...firefox.instance_1_23`).
const INSTANCE: &str = ".instance";

/// What the main thread knows about one visible player.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct PlayerInfo {
    /// The `dbus` thread's id for it (never reused).
    pub(crate) id: u64,
    /// The app's name for labels: its `Identity`, else the bus name's suffix.
    pub(crate) app: String,
    /// `Identity` ("" if it gave none), the title fallback.
    pub(crate) identity: String,
    /// `PlaybackStatus`: `Playing`, `Paused`, `Stopped`, or "" before any.
    pub(crate) status: String,
    /// `xesam:title` ("" if none).
    pub(crate) title: String,
    /// The first `xesam:artist` ("" if none).
    pub(crate) artist: String,
    /// `xesam:album` ("" if none).
    pub(crate) album: String,
    /// `CanGoNext`.
    pub(crate) can_next: bool,
    /// `CanGoPrevious`.
    pub(crate) can_prev: bool,
    /// `CanSeek`.
    pub(crate) can_seek: bool,
    /// `mpris:length` in microseconds (0 if unreported).
    pub(crate) length_us: i64,
}

impl PlayerInfo {
    /// Whether it's playing.
    pub(crate) fn is_playing(&self) -> bool {
        self.status == "Playing"
    }
}

/// What the main thread learns about the players.
#[derive(Debug)]
pub(crate) enum MediaEvent {
    /// The visible players changed (in order).
    Players(Vec<PlayerInfo>),
    /// The answer to [`MediaCommand::PollPositions`]: (player id, µs).
    Positions(Vec<(u64, i64)>),
}

/// What the main thread asks for.
#[derive(Debug)]
pub(crate) enum MediaCommand {
    /// Read every visible player's `Position`.
    PollPositions,
    /// `PlayPause` (every real player implements it).
    PlayPause {
        /// Which player.
        player: u64,
    },
    /// `Next`, if the player says it can.
    Next {
        /// Which player.
        player: u64,
    },
    /// `Previous`, if the player says it can.
    Previous {
        /// Which player.
        player: u64,
    },
    /// Jump to `position_us`, if the player can seek.
    Seek {
        /// Which player.
        player: u64,
        /// Where to, in µs from the start.
        position_us: i64,
    },
}

/// One tracked player, on the `dbus` thread.
#[derive(Debug, Default)]
struct Player {
    /// What the main thread is told (the id included).
    info: PlayerInfo,
    /// Its well-known bus name.
    bus_name: String,
    /// The unique name owning it: signals come from this.
    owner: String,
    /// The owning process (0 if the bus wouldn't say): see [`is_shadowed`].
    pid: u32,
    /// `mpris:trackid` ("" if none or not a valid object path): `SetPosition`
    /// names the track it's for.
    track_id: String,
    /// The last polled (or sought-to) position, µs.
    position_us: i64,
}

/// Every tracked player, in the order they appeared.
pub(crate) struct Players {
    /// The session bus.
    conn: Connection,
    /// Where the main thread hears about changes.
    events: channel::Sender<MediaEvent>,
    /// The players.
    list: Vec<Player>,
    /// The next player's id.
    next_id: u64,
    /// The visible list last sent.
    sent: Vec<PlayerInfo>,
}

impl Players {
    /// No players yet.
    pub(crate) const fn new(conn: Connection, events: channel::Sender<MediaEvent>) -> Self {
        Self {
            conn,
            events,
            list: Vec::new(),
            next_id: 0,
            sent: Vec::new(),
        }
    }

    /// Adds every player already on the bus.
    pub(crate) async fn start(&mut self) {
        let names = self
            .conn
            .call_method(
                Some("org.freedesktop.DBus"),
                "/org/freedesktop/DBus",
                Some("org.freedesktop.DBus"),
                "ListNames",
                &(),
            )
            .await
            .and_then(|reply| reply.body().deserialize::<Vec<String>>());
        match names {
            Ok(names) => {
                for name in names.iter().filter(|n| n.starts_with(NAME_PREFIX)) {
                    self.add(name, None).await;
                }
            }
            Err(err) => {
                eprintln!("stickBar: failed to list D-Bus names for MPRIS discovery ({err})");
            }
        }
        self.publish();
    }

    /// `NameOwnerChanged(name, _, new_owner)`: a player appeared or quit.
    pub(crate) async fn owner_changed(&mut self, name: &str, new_owner: &str) {
        if !name.starts_with(NAME_PREFIX) {
            return;
        }
        if new_owner.is_empty() {
            self.list.retain(|p| p.bus_name != name);
        } else {
            self.add(name, Some(new_owner)).await;
        }
        self.publish();
    }

    /// Starts tracking `bus_name` (owned by `owner`, looked up if `None`):
    /// its pid, its properties, and its `Identity`.
    async fn add(&mut self, bus_name: &str, owner: Option<&str>) {
        if bus_name.contains("playerctld") || self.list.iter().any(|p| p.bus_name == bus_name) {
            return;
        }
        let owner = match owner {
            Some(owner) => owner.to_owned(),
            None => self
                .bus_call::<String>("GetNameOwner", bus_name)
                .await
                .unwrap_or_default(),
        };
        // Best effort: an unknown pid (0) just never hides anything.
        let pid = self
            .bus_call::<u32>("GetConnectionUnixProcessID", bus_name)
            .await
            .unwrap_or(0);
        let mut player = Player {
            info: PlayerInfo {
                id: self.next_id,
                ..PlayerInfo::default()
            },
            bus_name: bus_name.to_owned(),
            owner,
            pid,
            ..Player::default()
        };
        self.next_id += 1;
        match self.get_all(bus_name).await {
            Ok(props) => apply_properties(&mut player, &props),
            Err(err) => {
                eprintln!("stickBar: failed to fetch MPRIS properties for {bus_name} ({err})");
            }
        }
        // On the root interface, not Player; it doesn't change. Best effort.
        if let Ok(identity) = self.get(bus_name, ROOT_INTERFACE, "Identity").await {
            player.info.identity = text_of(&identity);
        }
        player.info.app = app_name(&player.info.identity, bus_name);
        self.list.push(player);
    }

    /// A `PropertiesChanged` from some player's object.
    pub(crate) async fn properties_changed(&mut self, msg: &Message) {
        let header = msg.header();
        let Some(sender) = header.sender().map(|s| s.as_str().to_owned()) else {
            return;
        };
        let Ok((interface, changed, invalidated)) =
            msg.body()
                .deserialize::<(String, HashMap<String, OwnedValue>, Vec<String>)>()
        else {
            return;
        };
        if interface != PLAYER_INTERFACE {
            return; // the root interface (Identity...): nothing shown changes
        }
        // Every player this connection owns (normally one).
        let mut refetch = Vec::new();
        for player in self.list.iter_mut().filter(|p| p.owner == sender) {
            apply_properties(player, &changed);
            if !invalidated.is_empty() {
                refetch.push(player.bus_name.clone());
            }
        }
        for bus_name in refetch {
            if let Ok(props) = self.get_all(&bus_name).await
                && let Some(player) = self.list.iter_mut().find(|p| p.bus_name == bus_name)
            {
                apply_properties(player, &props);
            }
        }
        self.publish();
    }

    /// Runs a command from the main thread. The controls are sent without
    /// waiting for a reply, like C's `sd_bus_send`.
    pub(crate) async fn command(&mut self, command: MediaCommand) {
        let result = match command {
            MediaCommand::PollPositions => {
                self.poll_positions().await;
                Ok(())
            }
            MediaCommand::PlayPause { player } => self.call(player, "PlayPause", |_| true).await,
            MediaCommand::Next { player } => self.call(player, "Next", |p| p.info.can_next).await,
            MediaCommand::Previous { player } => {
                self.call(player, "Previous", |p| p.info.can_prev).await
            }
            MediaCommand::Seek {
                player,
                position_us,
            } => self.seek(player, position_us).await,
        };
        if let Err(err) = result {
            eprintln!("stickBar: media: a player control failed ({err})");
        }
    }

    /// Reads every visible player's position and tells the main thread.
    async fn poll_positions(&mut self) {
        let mut positions = Vec::new();
        let visible: Vec<usize> = visible_indices(&self.list);
        for i in visible {
            let Some(bus_name) = self.list.get(i).map(|p| p.bus_name.clone()) else {
                continue;
            };
            let Ok(value) = self.get(&bus_name, PLAYER_INTERFACE, "Position").await else {
                continue;
            };
            if let (Value::I64(position), Some(player)) =
                (unwrap_variant(&value), self.list.get_mut(i))
            {
                player.position_us = *position;
                positions.push((player.info.id, *position));
            }
        }
        if self.events.send(MediaEvent::Positions(positions)).is_err() {
            eprintln!("stickBar: media: the main loop stopped listening");
        }
    }

    /// Sends `method` (no arguments) to player `id`, if `allowed` says so.
    async fn call(
        &self,
        id: u64,
        method: &str,
        allowed: impl Fn(&Player) -> bool,
    ) -> zbus::Result<()> {
        let Some(player) = self.list.iter().find(|p| p.info.id == id) else {
            return Ok(());
        };
        if !allowed(player) {
            return Ok(());
        }
        let msg = player_call(&player.bus_name, method)?.build(&())?;
        self.conn.send(&msg).await
    }

    /// Jumps player `id` to `position_us`: `SetPosition` with the track's id
    /// (so a seek racing a track change is ignored, not applied to the next
    /// song), or without one a relative `Seek` from the last polled position.
    async fn seek(&mut self, id: u64, position_us: i64) -> zbus::Result<()> {
        let Some(player) = self.list.iter_mut().find(|p| p.info.id == id) else {
            return Ok(());
        };
        if !player.info.can_seek {
            return Ok(());
        }
        let position_us = position_us.max(0);
        let msg = if player.track_id.is_empty() {
            player_call(&player.bus_name, "Seek")?
                .build(&(position_us.saturating_sub(player.position_us)))?
        } else {
            let track = ObjectPath::try_from(player.track_id.as_str())?;
            player_call(&player.bus_name, "SetPosition")?.build(&(track, position_us))?
        };
        // The next poll corrects this if the player clamped or ignored it.
        player.position_us = position_us;
        self.conn.send(&msg).await
    }

    /// Sends the visible players to the main thread, if they changed.
    fn publish(&mut self) {
        let visible: Vec<PlayerInfo> = visible_indices(&self.list)
            .into_iter()
            .filter_map(|i| self.list.get(i).map(|p| p.info.clone()))
            .collect();
        if visible == self.sent {
            return;
        }
        self.sent.clone_from(&visible);
        if self.events.send(MediaEvent::Players(visible)).is_err() {
            eprintln!("stickBar: media: the main loop stopped listening");
        }
    }

    /// A method on the bus daemon taking one name.
    async fn bus_call<T>(&self, method: &str, name: &str) -> zbus::Result<T>
    where
        T: for<'d> zbus::zvariant::DynamicDeserialize<'d>,
    {
        let reply = self
            .conn
            .call_method(
                Some("org.freedesktop.DBus"),
                "/org/freedesktop/DBus",
                Some("org.freedesktop.DBus"),
                method,
                &name,
            )
            .await?;
        reply.body().deserialize::<T>()
    }

    /// Every property of `bus_name`'s player interface.
    async fn get_all(&self, bus_name: &str) -> zbus::Result<HashMap<String, OwnedValue>> {
        let reply = self
            .conn
            .call_method(
                Some(bus_name),
                OBJECT_PATH,
                Some(PROPERTIES),
                "GetAll",
                &PLAYER_INTERFACE,
            )
            .await?;
        reply.body().deserialize()
    }

    /// One property.
    async fn get(&self, bus_name: &str, interface: &str, name: &str) -> zbus::Result<OwnedValue> {
        let reply = self
            .conn
            .call_method(
                Some(bus_name),
                OBJECT_PATH,
                Some(PROPERTIES),
                "Get",
                &(interface, name),
            )
            .await?;
        reply.body().deserialize()
    }
}

/// A player-interface method call to `bus_name`, not waiting for a reply.
fn player_call<'a>(bus_name: &'a str, method: &'a str) -> zbus::Result<zbus::message::Builder<'a>> {
    Message::method_call(OBJECT_PATH, method)?
        .destination(bus_name)?
        .interface(PLAYER_INTERFACE)?
        .with_flags(Flags::NoReplyExpected)
}

/// The indices of the players shown (not shadowed), in order.
fn visible_indices(players: &[Player]) -> Vec<usize> {
    (0..players.len())
        .filter(|&i| !is_shadowed(players, i))
        .collect()
}

/// Whether player `i` is hidden behind another name of the same process.
///
/// One process can own several MPRIS names, and then they're the same
/// playback reported twice. Seen live with Spotify (Electron): one pid owned
/// `org.mpris.MediaPlayer2.spotify` and `...chromium.instance57585`, and the
/// mirror's title was *stale*. So per process one player shows: an app's own
/// name beats an `.instance` engine mirror, otherwise the earlier one wins. A
/// standalone browser still shows (nothing shares its pid), and an unknown
/// pid (0) never hides anything. Worked out on demand, so if Spotify's own
/// name goes, its mirror shows again.
fn is_shadowed(players: &[Player], i: usize) -> bool {
    let Some(p) = players.get(i) else {
        return false;
    };
    if p.pid == 0 {
        return false;
    }
    let p_instance = p.bus_name.contains(INSTANCE);
    players.iter().enumerate().any(|(j, o)| {
        if j == i || o.pid != p.pid {
            return false;
        }
        let o_instance = o.bus_name.contains(INSTANCE);
        (p_instance && !o_instance) || (p_instance == o_instance && j < i)
    })
}

/// The name to label a player with: its `Identity`, or the bus name's suffix
/// (`org.mpris.MediaPlayer2.vlc` -> `vlc`) minus any `.instance...` tail.
fn app_name(identity: &str, bus_name: &str) -> String {
    if !identity.is_empty() {
        return identity.to_owned();
    }
    let name = bus_name.strip_prefix(NAME_PREFIX).unwrap_or(bus_name);
    name.find(INSTANCE)
        .and_then(|end| name.get(..end))
        .unwrap_or(name)
        .to_owned()
}

/// Applies player-interface properties (from `GetAll` or `PropertiesChanged`).
fn apply_properties(player: &mut Player, props: &HashMap<String, OwnedValue>) {
    for (key, value) in props {
        let value = unwrap_variant(value);
        match (key.as_str(), value) {
            ("PlaybackStatus", _) => player.info.status = text_of(value),
            ("Metadata", Value::Dict(_)) => apply_metadata(player, value),
            ("CanGoNext", Value::Bool(b)) => player.info.can_next = *b,
            ("CanGoPrevious", Value::Bool(b)) => player.info.can_prev = *b,
            ("CanSeek", Value::Bool(b)) => player.info.can_seek = *b,
            _ => {}
        }
    }
}

/// Applies a `Metadata` dict: it replaces the whole track, so everything it
/// could set is cleared first.
fn apply_metadata(player: &mut Player, metadata: &Value<'_>) {
    let Value::Dict(dict) = metadata else {
        return;
    };
    player.info.title.clear();
    player.info.artist.clear();
    player.info.album.clear();
    player.info.length_us = 0;
    player.track_id.clear();
    for (key, value) in dict.iter() {
        let Value::Str(key) = key else {
            continue;
        };
        let value = unwrap_variant(value);
        match (key.as_str(), value) {
            ("xesam:title", _) => player.info.title = text_of(value),
            ("xesam:album", _) => player.info.album = text_of(value),
            // An array per spec (several credited artists): the first is kept.
            ("xesam:artist", Value::Array(artists)) => {
                player.info.artist = artists.iter().next().map(text_of).unwrap_or_default();
            }
            // The spec says an object path; some players send a string. It
            // goes back out in SetPosition, so it must be a valid path.
            ("mpris:trackid", Value::ObjectPath(path)) => {
                path.as_str().clone_into(&mut player.track_id);
            }
            ("mpris:trackid", Value::Str(id)) if ObjectPath::try_from(id.as_str()).is_ok() => {
                id.as_str().clone_into(&mut player.track_id);
            }
            ("mpris:length", Value::I64(length)) => player.info.length_us = *length,
            // The spec says int64, but Spotify sends uint64 (checked live):
            // accepting only int64 left its timeline empty (BUG.md "Media").
            ("mpris:length", Value::U64(length)) => {
                player.info.length_us = i64::try_from(*length).unwrap_or(i64::MAX);
            }
            _ => {}
        }
    }
}

#[cfg(test)]
mod tests {
    use std::collections::HashMap;

    use zbus::zvariant::{ObjectPath, OwnedValue, Value};

    use super::{Player, app_name, apply_properties, is_shadowed};

    /// A player with a bus name and pid.
    fn player(bus_name: &str, pid: u32) -> Player {
        Player {
            bus_name: bus_name.to_owned(),
            pid,
            ..Player::default()
        }
    }

    #[test]
    fn one_player_per_process() {
        let players = [
            player("org.mpris.MediaPlayer2.chromium.instance57585", 57585),
            player("org.mpris.MediaPlayer2.spotify", 57585),
            player("org.mpris.MediaPlayer2.vlc", 0),
            player("org.mpris.MediaPlayer2.vlc2", 0),
            player("org.mpris.MediaPlayer2.a", 7),
            player("org.mpris.MediaPlayer2.b", 7),
        ];
        // The engine mirror loses to the app's own name, whatever the order.
        assert!(is_shadowed(&players, 0));
        assert!(!is_shadowed(&players, 1));
        // Unknown pids never hide anything.
        assert!(!is_shadowed(&players, 2) && !is_shadowed(&players, 3));
        // Same kind: the earlier one wins.
        assert!(!is_shadowed(&players, 4) && is_shadowed(&players, 5));
    }

    #[test]
    fn app_names() {
        assert_eq!(
            app_name("Spotify", "org.mpris.MediaPlayer2.spotify"),
            "Spotify"
        );
        assert_eq!(app_name("", "org.mpris.MediaPlayer2.vlc"), "vlc");
        assert_eq!(
            app_name("", "org.mpris.MediaPlayer2.chromium.instance9073"),
            "chromium"
        );
    }

    /// `props` as `GetAll` would send them.
    fn props(entries: Vec<(&str, Value<'static>)>) -> HashMap<String, OwnedValue> {
        entries
            .into_iter()
            .map(|(k, v)| (k.to_owned(), OwnedValue::try_from(v).unwrap()))
            .collect()
    }

    /// A metadata dict.
    fn metadata(entries: Vec<(&str, Value<'static>)>) -> Value<'static> {
        let dict: HashMap<String, Value<'static>> = entries
            .into_iter()
            .map(|(k, v)| (k.to_owned(), Value::Value(Box::new(v))))
            .collect();
        Value::from(dict)
    }

    #[test]
    fn metadata_parsing() {
        let mut p = player("org.mpris.MediaPlayer2.x", 1);
        apply_properties(
            &mut p,
            &props(vec![
                ("PlaybackStatus", Value::from("Playing")),
                ("CanSeek", Value::from(true)),
                (
                    "Metadata",
                    metadata(vec![
                        ("xesam:title", Value::from("Song")),
                        ("xesam:artist", Value::from(vec!["First", "Second"])),
                        ("mpris:length", Value::from(200_000_000_u64)),
                        (
                            "mpris:trackid",
                            Value::from(ObjectPath::try_from("/t/1").unwrap()),
                        ),
                    ]),
                ),
            ]),
        );
        assert!(p.info.is_playing() && p.info.can_seek);
        assert_eq!(
            (p.info.title.as_str(), p.info.artist.as_str()),
            ("Song", "First")
        );
        assert_eq!(p.info.length_us, 200_000_000);
        assert_eq!(p.track_id, "/t/1");

        // A new track replaces everything; a string track id that isn't a
        // path is dropped; int64 lengths work too.
        apply_properties(
            &mut p,
            &props(vec![(
                "Metadata",
                metadata(vec![
                    ("xesam:album", Value::from("Album")),
                    ("mpris:trackid", Value::from("not a path")),
                    ("mpris:length", Value::from(5_i64)),
                ]),
            )]),
        );
        assert!(p.info.title.is_empty() && p.info.artist.is_empty() && p.track_id.is_empty());
        assert_eq!((p.info.album.as_str(), p.info.length_us), ("Album", 5));
        assert!(p.info.is_playing()); // status untouched by Metadata
    }
}
