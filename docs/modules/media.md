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
- **`PropertiesChanged` is applied from the signal itself**: its changed-properties
  dictionary goes through the same `dbus/dbus_dict.c` walker and `player_entry()`
  as `GetAll`; only names sent as "invalidated" (no value) trigger a `GetAll`. It
  used to re-fetch with a blocking `GetAll` + `Get(Identity)` per signal -- browsers
  send it for every volume/rate/metadata tweak, and a hung player could stall the
  whole bar for sd-bus's 25s timeout. The bar is marked dirty (and
  `mpris_change_count()` bumped, which is what refreshes an open media dropdown)
  only if something shown changed: status, title, artist, album, length, or the
  prev/next capabilities.
- **`Identity` (the player's own display name, used as a title fallback) lives on the
  root `org.mpris.MediaPlayer2` interface, not `Player`** — fetched once, with a
  separate `sd_bus_get_property()` call, when the player appears (it doesn't
  change). Best-effort: a player that fails to answer it just keeps an empty
  identity.
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
    mirror `MediaPopup.qml`'s own `maxSaneDuration`/`formatTime()`.
  - **Click-to-seek on the timeline**: a left click anywhere along the meter's
    width, over the whole 14px row, jumps that far into the track
    (`mpris_player_seek_to()`). It sends `SetPosition(trackid, pos)` with the
    track's `mpris:trackid`, or a relative `Seek(pos − last polled position)` if
    the player reports no track id. Spotify sends the id as a string (`s`), not
    the spec's object path (`o`), checked live; both are accepted if the value
    is a valid path. Only when the player's `CanSeek` is true and the length is
    sane; otherwise the row isn't clickable and doesn't light up. One of the
    user's Chromium players reports `CanSeek false`. Hovering a seekable
    timeline lightens its empty segments, like the chips. The cached position
    is set to the target at once, so the meter jumps before the next 1s
    `Position` poll, which corrects it if the player clamped or ignored the
    seek. Fire-and-forget like the chips; the dropdown stays open.
- **Deliberately out of scope**: no per-player volume/mute control (would need
  client-side mute-state bookkeeping like the reference's own `_mutedVolumes`, judged
  not essential for v1).

## In the Rust port

- **Files:** `src/mpris/mod.rs` (the players, on the `dbus` thread), `src/modules/media/mod.rs`
  (`Media` and the centred box), `src/modules/media/popup.rs` (`MediaPopup`). The box and the
  dropdown are pixel-identical to C's with one and two players: open, chip hovered, timeline
  hovered. The visualizer is M14, so the cards have the C bar's no-graph layout, the same one
  every test bar uses (`STICKBAR_TEST_NO_VISUALIZER=1`).
- **MPRIS shares the tray's `dbus` thread and connection** (`docs/modules/tray.md`): one more
  stream (`PropertiesChanged` on `/org/mpris/MediaPlayer2`), the `NameOwnerChanged` stream the
  tray already had, a second calloop channel (`MediaEvent`, to `App::media_event`), and
  `Command::Media`. The main loop never waits for a player.
- **The main thread gets the visible players' `PlayerInfo`s, and only when that list
  changed.** That's C's "dirty only if something shown changed", for free: a browser's
  `PropertiesChanged` storms (volume, rate) cost no redraw.
- **Rule: match a player's signals by the sender's unique name.** Every player shares one
  object path, and C subscribed per player with the well-known name as the match's sender,
  which sd-bus can't check locally. So every subscription got every player's
  `PropertiesChanged`, and one player's status or track showed on all of them (BUG.md "Media").
  Here each player keeps its owner's unique name, and a signal applies to the players that
  connection owns.
- **Position is polled asynchronously:** `App::clock_tick` sends `PollPositions` while the
  dropdown is open (once a second, plus once on open), and the answer repaints. No `Position`
  is read while it's closed (checked with `dbus-monitor`: 5 reads in an 11 s window with it open
  for 4 s). C read the positions synchronously before the first frame; here the first frame
  shows the last known position for the few ms until the answer.
- **Seeking:** the target position is set on the main side at once (C's optimistic jump), and
  the thread keeps it as its "last polled" position for a relative `Seek`.
- **No fixed caps:** players and cards are `Vec`s (C: 16).

## Spectrum visualizer

No reference equivalent -- added per user request ("an audio wave thingy in the
middle module"), then reshaped by several more: **only in the media dropdown**
(not the bar, so a closed dropdown costs nothing), **one graph per player card**
showing *that player's own* audio, drawn **like the network dropdown's graph**:
braille dots (`render/braille_graph.c`), the **left channel rising** toward a
centre line and the **right channel hanging** below it, one dot column per
frequency band, soft faded ends.

- **Layout** (`media_popup.c`): every card (up to `VISUALIZER_MAX_SOURCES` = 4)
  gets the graph row, playing or not, so play/pause never changes the popup's
  height. Panel = card inner width (340px), 41 braille cells = **82 dot columns
  = 82 bands**, 4 cells = **16 dot rows per half**, a 5px centre gap with a 1px
  `surface1` line (the network graph's `GRAPH_CENTER_GAP`). Colours by height:
  **L** yellow -> peach -> red, **R** mauve -> pink -> red; a lone `text` dot
  marks each column's recent peak (held 0.5s, then falling slower than the
  column); the ends fade into the `mantle` panel over 28px.
- **Runs only while the dropdown is open, and per card only while its player is
  Playing.** `main.c` calls `media_popup_sync_visualizer()` once per loop
  iteration, which calls `visualizer_set_source(slot, target, active)` per card;
  a slot's stream is created the first time it's turned on and paused when
  off. Closed dropdown: every slot off, levels zeroed, timer disarmed. Measured
  (release, two players playing, dropdown closed): identical to a bar with the
  visualizer disabled (0.13% / 0.20% for both).
- **Each player's own stream** (`pipewire_find_playback_for_pid()` in
  `pipewire/pipewire_client.c`): the MPRIS owner's pid (`mpris_player_pid()`) is
  matched against each `Stream/Output/Audio` node's client's pid -- **or any of
  that pid's ancestors** (`/proc/<pid>/status` `PPid:` walk), because browsers
  play audio from a child process. The client pid has to come from the bound
  client's info (`application.process.id`): the registry global only has
  `pipewire.sec.pid`, which for everything going through pipewire-pulse is the
  daemon's own pid (seen live: Steam, browsers, the bar's own libpulse client
  all showed pid 1515). Prefers a running stream (Spotify keeps an idle one).
  Re-resolved on open, when a card's player changes, and when PipeWire reports a
  playback stream or a client pid changed (`PIPEWIRE_PLAYBACK_CHANGED` ->
  `media_popup_streams_changed()`), never per frame. No match -> the card
  shows the default output's mix (`VISUALIZER_TARGET_MIX`, the sink monitor with
  `stream.capture.sink`).
- **Capture** (`visualizer/visualizer.c`): one `pw_thread_loop` + core, connected
  on the first capture (a bar whose dropdown is never opened during playback has no
  visualizer thread or connection at all), then kept. One `pw_stream` per active
  slot, destroyed when the slot turns off (not paused: a paused stream stays a node
  in the graph). Stereo F32 FL/FR at 48kHz, `node.passive`. Per-app
  streams set `target.object` to the node's `object.serial` **plus
  `node.dont-fallback`/`node.dont-reconnect`**: without them, a target that's gone
  (the app closed its stream between tracks) makes the session manager link a
  capture stream to the default *source* -- the microphone.
- **Analysis** (`visualizer/spectrum.c`, no PipeWire types): 4096-sample Hann
  window (11.7Hz bins; 2048 left the bottom dozen of 82 log bands sharing bins),
  as a half-size complex FFT over the samples packed in pairs plus an O(N)
  untangling pass, precomputed twiddles. 82 log-spaced bands 50Hz-12kHz, peak
  bin per band, dBFS with a +3dB/octave tilt around 500Hz, -62..-14dB -> 0..1.
  Two FFTs per playing card per frame (L, R). Smoothing (fast attack, fixed
  fall) and peak hold are derived from `VISUALIZER_FPS` (15, the user's pick).
- **A Playing player doesn't mean audio is flowing.** Between tracks, or with a
  stalled stream, PipeWire stops delivering buffers, which froze the graph
  mid-pose. After 2 frames (~133ms) with no new samples the rings count as
  silence.
- **Drawing is memcpy, not cairo.** Three whole-panel images are rendered once
  per open: the empty panel, the panel with every dot lit, and the same with
  every dot in the peak colour, each with the fade baked in. A frame copies the
  empty panel, then per column the lit rows' rectangle out of the lit image
  (`braille_graph_column_rect()` gives it: dot centres on half pixels, 3px pitch,
  so a column is an exact whole-pixel band), and the peak dot's out of the peak
  image. Each graph is its own subsurface (`bar/strip_surface.c`, 340x129), so
  the popup isn't touched; a frame where no dot changed isn't committed. The
  popup also paints each graph under its subsurface (invisible) so `snapshot`
  PNGs show it.
- **Cost, dropdown open** (release, one output, synthetic noise players):
  ~1.2-1.3% with one playing player, ~1.5-1.9% with two (an open dropdown
  without graphs: ~0.3-0.5%). Per frame and playing card: analysis ~0.2ms, drawing
  ~0.13ms; plus the capture thread (~0.15-0.2%) and the dropdown's existing 1s
  full repaint (~2.4ms). How the drawing got there: per-dot antialiased arcs
  (what the network graph's `braille_graph_draw()` does) would be thousands per
  frame; per-column cairo fills from a sprite cost ~0.5ms/frame (call overhead,
  not pixels); batched into one path ~0.36ms; plain memcpy from pre-composited
  panels ~0.13ms. Earlier steps, while it was still bars: re-running the box's
  Pango layout per frame cost ~7%; 30 -> 15fps; subsurfaces instead of repainting
  inside the bar/popup; the real-input FFT.
- **The privacy indicator ignores it**: every slot's node is a
  `Stream/Input/Audio` named `VISUALIZER_NODE_NAME`, which `pipewire_client.c`
  skips.
- If PipeWire or `wl_subcompositor` is unavailable the dropdown has no graph
  rows, laid out exactly as before.
- **Tests never use real audio** (per user request): `test_visualizer.py` runs
  the bar with `STICKBAR_TEST_VISUALIZER_SYNTH=1` -- no PipeWire connection, no
  capture streams; an active slot analyses sine tones set over the test-control
  channel (`vis-tone <slot> <channel> <hz>`), and everything from there on is
  the real path. `STICKBAR_TEST_VISUALIZER_ONLY` limits capture to its fake
  players. The flip side: player <-> stream matching, the capture itself and
  the privacy filter aren't covered by tests -- they were checked live during
  development: Spotify matched to its running stream by pid, the Helium
  browser (Chromium-based) to its stream via the ancestor walk (a synthetic-mode
  bar still resolves targets, so this needs no capture); and, in an earlier
  version of the test that captured fake players' tones from a null sink,
  child-process players matched, L/R tones landed in the right halves, and the
  privacy icon stayed hidden. Every other test bar runs with the visualizer off
  (`STICKBAR_TEST_NO_VISUALIZER=1`, `tests/lib/common.py`).
