# Media (MPRIS)

The reference bar's `MediaWidget.qml` lives centered in the bar's own `centerLayout`
(the only widget there) and wraps `Quickshell.Services.Mpris` via a `MediaPlayer.qml`
singleton. This project's `mpris/mpris_client.c` talks to the same
`org.mpris.MediaPlayer2.*` D-Bus protocol directly via sd-bus, reusing the tray's own
shared session-bus connection (`dbus/dbus_loop.c`) rather than opening a second one —
structurally the mpris-domain twin of `dbus/`'s SNI plumbing (`mpris_client.c`'s
tracked-player table mirrors `sni_registry.c`'s `SniItem` array almost exactly: a
fixed, non-relocated array so a slot's address stays valid as a `sd_bus_match_signal()`
userdata pointer, `in_use`-gated iteration, `_count()`/`_get(index)` accessors).

- **Player discovery**: `ListNames` on `org.freedesktop.DBus` at startup, filtered to
  the `org.mpris.MediaPlayer2.` prefix; a global `NameOwnerChanged` subscription
  (filtered *inside* the callback by that same prefix, not via a raw match-string
  API this codebase doesn't otherwise use) catches players appearing/disappearing
  afterward — same "global match, filter inside the callback" shape
  `dbus/sni_watcher_service.c`'s own `on_name_owner_changed()` already uses for the
  identical reason (D-Bus match rules can't filter an arbitrary name *prefix*
  without that raw-string API). `playerctld` is excluded by a substring check on the
  bus name, matching `MediaPlayer.qml`'s own filter — it's a shadow/mirror service
  that duplicates whatever's actually playing under its own MPRIS name, so including
  it would show every real player twice.
- **One visible player per process** (`is_shadowed()` in `mpris_client.c`), since some
  apps own two MPRIS names, and the mirror's metadata can be stale. Each player's owner PID is read once in
  `add_player()` via `sd_bus_get_name_creds(SD_BUS_CREDS_PID)`; among players
  sharing a PID, an app's own name beats a `.instance` browser-engine name,
  otherwise the lower slot wins. `mpris_player_count()`/`_get()`/
  `mpris_active_player()` only see visible players, so the bar widget and the
  popup needed no changes. Computed on demand, so if the preferred name goes
  away the other one reappears; a standalone browser (nothing else on its PID)
  is never hidden, and an unreadable PID (0) never hides anything.
- **`mpris:length` is accepted as `t` (uint64) as well as `x`**: the spec says int64, but
  Spotify sends uint64. `Position` is `x` for every player checked (Spotify, Chromium),
  so only length needed this.
- **Every MPRIS player lives at the same fixed object path**
  (`/org/mpris/MediaPlayer2`), unlike SNI items (which each declare their own path) —
  no per-item path discovery is needed here at all.
  `mpris_client.c` subscribes to that path's `org.freedesktop.DBus.Properties`
  interface with `member=NULL` (matches every signal from that interface, which in
  practice is only ever `PropertiesChanged`) — same "match broadly, dispatch by
  member name inside the callback" convention `sni_registry.c`'s own per-item
  subscription already uses.
- **On `PropertiesChanged`, the whole `Player`-interface property set is re-fetched
  via a fresh `GetAll` call, rather than parsing the signal's own incremental
  changed-properties payload.** Simpler and more robust than tracking partial
  updates, at the cost of one extra round trip per change — the same tradeoff
  `sni_registry.c`'s `refresh_icon()` already makes for `NewIcon` (re-fetch the one
  property that matters rather than parse what the signal handed over). Acceptable
  here since `PropertiesChanged` fires relatively rarely (track changes, play/pause
  toggles), not on a hot path.
- **`Identity` (the player's own display name, used as a title fallback) lives on the
  root `org.mpris.MediaPlayer2` interface, not `Player`** — fetched as a second,
  separate `sd_bus_get_property()` call after the `Player` GetAll. Best-effort: a
  player that fails to answer it just keeps whatever identity string (possibly
  empty) it already had, rather than treating the whole property fetch as failed.
- **`xesam:artist` is an array of strings per spec** (a track can have multiple
  credited artists) — only the first is kept, matching `trackText()`'s own
  single-artist display. The D-Bus reader still drains every remaining array entry
  before calling `sd_bus_message_exit_container()`, same reasoning
  `sni_registry.c`'s `convert_best_pixmap_from_message()` already documents for
  draining every `IconPixmap` entry rather than stopping early once the best one is
  found — leaving unread bytes in a `sd_bus_message` before exiting its container is
  not safe to skip.
- **Fire-and-forget method calls** (`PlayPause`/`Next`/`Previous`, via
  `mpris_player_toggle_playing()`/`_next()`/`_previous()`): `sd_bus_send()`, not
  `sd_bus_call_method()`, for the same reason `dbus/sni_registry.c`'s
  `sni_item_activate()` already documents — this runs from a UI click handler inside
  the bar's single-threaded event loop, and blocking on a remote player's reply
  would freeze the whole bar until it responds. `mpris_player_toggle_playing()`
  always sends `PlayPause` unconditionally rather than replicating the reference
  `togglePlaying()`'s `canTogglePlaying`-gated `Play()`/`Pause()` fallback --
  `PlayPause` is spec-optional but implemented by essentially every real player, and
  it only matters for a player that both lacks it *and* reports `CanPlay`/`CanPause`
  as false, so the extra capability branching wasn't judged worth it for v1.
- **The bar widget (`modules/media/media.c`) is horizontally centered in the bar**,
  the only module in this codebase that isn't anchored to either the left or right
  region — matches the reference's own `centerLayout` /
  `anchors.horizontalCenter: parent.horizontalCenter`. It shows the *active* player
  (`mpris_active_player()`: first currently-`Playing` tracked player, or the first
  tracked player at all, or nothing) and collapses to zero width when there's no
  player at all, matching `MediaWidget.qml`'s own `visible: player !== null`.
  It's drawn *after* both side regions and is clamped between them
  (`render.c` passes the end of the left region and the start of the right one):
  the box stays centered, the title truncates further if the narrower side
  doesn't leave room, and if not even the icon plus `...` fits it collapses
  like the no-player case.
- **The right-click dropdown lists every tracked player**, not just the active one.
  It replaces the reference's hover-triggered `MediaPopup`, and closes when the last
  player goes.
- **Dropdown contents** (fixed width, so a track change never resizes it):
  - **Header**: `Media · N players` and a pill for the liveliest state
    across players (`playing` green > `paused` yellow > `stopped`).
  - **One `base` card per player**; the active player (the one the bar box
    shows, `mpris_active_player()`) gets a 2px accent border, like the
    calendar's current month. Top to bottom: app name in caps + coloured
    status word, the title (bold, truncated), `artist · album` (line
    omitted if both are empty -- `xesam:album` is now parsed; helium sends
    an empty one), a `meter_bar` timeline with `pos / length` (`h:mm:ss`
    past an hour; no sane length -> empty meter, elapsed time only), and
    three button chips. Hover is per chip now, not the whole row.
  - Verified with real `ydotool` input against helium plus a fake MPRIS
    player (a ~70-line Gio script owning `org.mpris.MediaPlayer2.*`, logging
    calls and toggling its status -- a good way to test controls without
    touching a real stream): growth recreated the surface live, a click on
    the fake's play chip sent `PlayPause` and flipped card + pill, hover
    lit only the chip under the pointer. `tests/test_media.py` covers the shrink.
  - **App-name label** (`mpris_player_app_name()`: the player's MPRIS `Identity`, e.g. "Spotify",
    or the bus-name suffix minus any `.instance…` tail if that's empty): with several
    players open, the track text alone doesn't say which app a card is.
  - **No thumbnails (removed per explicit user request).** There used to be a
    40px cover-art column (`mpris/mpris_art.c`, decoding `file://` art via
    gdk-pixbuf), but Spotify's and most browsers' `mpris:artUrl` is an
    `https://` CDN link and this project has no HTTP client, so in practice it
    always showed a placeholder square. Downloading art (libcurl in a
    background thread + a disk cache) was offered and declined, so
    `mpris_art.c` and the gdk-pixbuf dependency were deleted, and the art URL
    is no longer parsed at all.
  - **Position/duration timeline**: MPRIS's `Position` property isn't
    push-notified via `PropertiesChanged` per spec (unlike everything else this
    popup shows), so it needs actual polling while something's watching it, same
    as the reference `MediaPopup.qml`'s own `Timer { running: root.visible }`
    re-triggering `positionChanged()`. Rather than add a new poll source,
    `media_popup_tick()` piggybacks on `main.c`'s already-ticking 1-second clock
    timerfd (a no-op unless the popup is actually open) and re-fetches every
    visible row's player's `Position` via a synchronous `sd_bus_get_property()`
    call each second. `mpris/mpris_client.c` deliberately does *not* poll
    `Position` on its own regular schedule — only `media_popup_tick()` (and
    `media_popup_open()`, once, for the popup's very first frame) calls
    `mpris_player_refresh_position()`, so a player nobody's looking at isn't
    polled at all (the bar box shows no position, like the reference's
    `MediaWidget.qml`). The "24h sentinel" `mpris:length` heuristic and "m:ss" formatting
    mirror `MediaPopup.qml`'s own `maxSaneDuration`/`formatTime()`. **Read-only, no
    click-to-seek** — `SetPosition()` needs the track's own `mpris:trackid`
    object path, which isn't cached anywhere (only the fields this popup
    actually displays are), and adding it purely to support seeking wasn't
    judged worth it here.
- **Deliberately out of scope**: no per-player volume/mute control (would need
  client-side mute-state bookkeeping like the reference's own `_mutedVolumes`, judged
  not essential for v1).
