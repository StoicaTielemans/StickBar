# Media (MPRIS), the Dashboard dropdown, the spectrum visualizer

Files: `src/mpris/mod.rs` (the players, on the `dbus` thread), `src/modules/media/mod.rs`
(`Media` and the centred box), `src/modules/media/popup.rs` (`MediaPopup`, the Dashboard
dropdown), `src/modules/media/graphs.rs` (the spectrum panels), and the dropdown's other
cards: `eq_section.rs` ([eq.md](eq.md)) and `shelly_section.rs` ([shelly.md](shelly.md)).

The reference's `MediaWidget.qml` sits centred in the bar and wraps
`Quickshell.Services.Mpris`; this bar speaks the `org.mpris.MediaPlayer2.*` D-Bus protocol
itself.

## Players (`src/mpris/mod.rs`)

- **MPRIS shares the tray's `dbus` thread and connection** ([tray.md](tray.md)): one more
  stream (`PropertiesChanged` on `/org/mpris/MediaPlayer2`), the `NameOwnerChanged` stream the
  tray already had, a calloop channel (`MediaEvent`, to `App::media_event`) and
  `Command::Media` back. The main loop never waits for a player; calls time out after 3 s.
- **Discovery**: `ListNames` at startup, filtered to the `org.mpris.MediaPlayer2.` prefix,
  then `NameOwnerChanged` (filtered by the same prefix) for players that come and go.
  `playerctld` is skipped (a mirror of whatever plays, which would show every player twice),
  as in the reference's `MediaPlayer.qml`.
- **Match a player's signals by the sender's unique name.** Every player shares the one
  object path `/org/mpris/MediaPlayer2`, so a signal applies to the players that connection
  owns. Matching by well-known name once showed one player's track on all of them
  ([BUG.md](../../BUG.md) "Media").
- **One visible player per process** (`is_shadowed`): one process can own two MPRIS names
  (Spotify owns its own and a `chromium.instance…` mirror, whose title can be stale). Per
  owner pid, an app's own name beats an `.instance` name, otherwise the earlier one wins. A
  standalone browser still shows, an unknown pid (0) never hides anything, and if the
  preferred name goes, the other shows again.
- **`PropertiesChanged` is applied from the signal itself**; only "invalidated" names
  (sent without a value) trigger a `GetAll`. **The main thread gets the visible players'
  `PlayerInfo`s only when that list changed**, so a browser's storms (volume, rate) cost no
  redraw. What's shown: status, title, artist, album, length, prev/next capabilities.
- **`Identity`** (the app's own name, e.g. "Spotify") is on the root
  `org.mpris.MediaPlayer2` interface, read once when the player appears. It labels the card
  (falling back to the bus-name suffix minus any `.instance…` tail) and is the title fallback.
- **Wire quirks**: `mpris:length` is accepted as `t` (uint64) as well as the spec's `x`
  (Spotify sends uint64); `xesam:artist` is an array and only the first is kept, like the
  reference's `trackText()`.
- **Controls are sent with `NoReplyExpected`**: `PlayPause`, `Next`, `Previous`.
  `PlayPause` is sent unconditionally (spec-optional, but every real player has it).

## The box

- **Centred in the bar**, the only box not in the left or right region (the reference's
  `centerLayout`). It's drawn after both side regions and clamped between them: the title
  truncates if the narrower side leaves no room.
- **It shows the active player** (`Media::active`: the first playing, else the first). A left
  click plays/pauses it, a **middle click skips to the next track** (the player ignores it if
  it can't go next), and a right click opens the dropdown.
- **It never goes away** (the user's request): with no player it shows `▶  No media playing`
  until something has played since the bar started, then `▶  <artist - title>` of the last
  thing shown (`Media::idle_text`, memory only). Same yellow box; left and middle click do
  nothing, right click opens the dropdown, so the EQ and plugs are always reachable.

## The dropdown

- **Layout**: a full-width `Dashboard` header (754 px wide). Left column: the EQ card, then
  the smart-plug card with a `shelly.conf`. Right column: `Media · N players` with a pill for
  the liveliest state (`playing` green > `paused` yellow > `stopped`), then one card per
  player. The height is the taller column's. It stays open (and as wide) when the last player
  goes: the player column keeps its header (`no players`). Widths are fixed, so a track
  change never resizes it; a player coming or going does ([popups.md](../popups.md)).
- **Player cards**: the active player's card gets a 2 px accent border. Top to bottom: app
  name in caps and a coloured status word, the title (bold, truncated), `artist · album`
  (left out if both are empty), a `meter_bar` timeline with `pos / length` (`h:mm:ss` past an
  hour; a length over 24 h counts as none: empty meter, elapsed time only, the reference's
  `maxSaneDuration`), then prev / play-pause / next chips, each with its own hover.
- **No cover art** (the user's choice): most players' `mpris:artUrl` is an `https://` link and
  downloading art was declined, so the art URL isn't read.
- **Position is polled only while the dropdown is open**: MPRIS doesn't push `Position`.
  `App::clock_tick` sends `PollPositions` once a second (plus once on open), asynchronously,
  and the answer repaints. Nothing is read while it's closed (checked with `dbus-monitor`).
  The first frame shows the last known position for the few ms until the answer.
- **Click-to-seek**: a left click along the timeline row jumps that far in. It sends
  `SetPosition(trackid, pos)` with `mpris:trackid` (Spotify sends it as a string, not the
  spec's object path; both are accepted if the value is a valid path), or a relative
  `Seek(pos − last polled)` without a track id. Only when `CanSeek` is true and the length is
  sane (one of the user's Chromium players says `CanSeek false`); otherwise the row isn't
  clickable. The shown position jumps at once and the next poll corrects it.
- **Out of scope**: per-player volume/mute.

## Spectrum visualizer

No reference equivalent, added at the user's request and reshaped by several more: **only
in the dropdown** (a closed dropdown costs nothing), **one graph per player card** showing
that player's own audio, drawn like the network graph: braille dots, the **left channel
rising** toward a centre line and the **right channel hanging** below it, one dot column per
frequency band, faded ends.

Files: `src/visualizer/mod.rs` (`Visualizer`: slots, smoothing, peaks, synth mode, `pump`),
`src/visualizer/spectrum.rs` (FFT and bands, pure), `src/modules/media/graphs.rs`,
`src/bar/strip_surface.rs`, `src/pipewire/capture.rs` (capture streams, rings, the `/proc`
ancestry walk), and the playback/client tracking in `src/pipewire/{mod,graph}.rs`.

- **Layout**: every card (up to `visualizer::MAX_SOURCES` = 4) gets the graph row, playing or
  not, so play/pause never changes the height. Panel = card inner width (340 px): 41 braille
  cells = **82 dot columns = 82 bands**, 16 dot rows per half, a 5 px centre gap with a 1 px
  `surface1` line. Colours by height: **L** yellow → peach → red, **R** mauve → pink → red; a
  `text` dot marks each column's recent peak (held 0.5 s, then falling slower than the
  column); the ends fade into the `mantle` panel over 28 px.
- **Runs only while the dropdown is open, and per card only while its player plays.** A
  closed dropdown means every slot off, levels zeroed, no timer.
- **Each player's own stream**: the MPRIS owner's pid is matched against each
  `Stream/Output/Audio` node's client pid, **or any of that pid's ancestors** (a `PPid:` walk
  in `/proc`), because browsers play audio from a child process. The client pid must come from
  the bound client's `application.process.id`: the registry global's `pipewire.sec.pid` is
  pipewire-pulse's own pid for everything going through it. A running stream is preferred
  (Spotify keeps an idle one). Re-resolved on the `pipewire` thread when playback streams
  start, stop, come or go, or a client's pid becomes known, never per frame. No match: the
  default output's mix (`capture::TARGET_MIX`, the sink monitor).
- **Capture runs on the privacy indicator's `pipewire` thread** ([privacy.md](privacy.md)), no
  second connection. The main thread sends `CaptureCommand::Slot { slot, want: (player pid,
  rings) }`. One `pw_stream` per active slot, destroyed when the slot turns off (a paused
  stream stays a node in the graph), recreated only when its target changes. Stereo F32 at
  48 kHz, `node.passive`. Per-app streams set `target.object` to the node's `object.serial`
  **plus `node.dont-fallback`/`node.dont-reconnect`**: without them, a target that's gone (the
  app closed its stream between tracks) makes the session manager link the capture to the
  default *source*, the microphone.
- **The rings** are an `Arc<Mutex<Rings>>` per slot, written by the `process` callback on the
  `pipewire` thread and read once per frame by the main thread: the one genuinely shared
  buffer.
- **Analysis** (`spectrum.rs`): a 4096-sample Hann window (11.7 Hz bins; 2048 left the bottom
  dozen bands sharing bins), `realfft`'s real-input FFT. 82 log-spaced bands from 50 Hz to
  12 kHz, peak bin per band, dBFS with a +3 dB/octave tilt around 500 Hz, −62..−14 dB → 0..1.
  Two FFTs per playing card per frame (L, R). Smoothing (fast attack, fixed fall) and peak
  hold follow `visualizer::FPS` (15, the user's pick; 30 cost twice the CPU).
- **A playing player doesn't mean audio flows.** Between tracks PipeWire stops delivering
  buffers, which froze the graph mid-pose; after 2 frames (~133 ms) without new samples the
  rings count as silence.
- **Drawing is slice copies, not cairo.** Three panel images are rendered once per open: the
  empty panel, every dot lit, every dot in the peak colour, each with the fade baked in. A
  frame copies the empty panel, then per column the lit rows' rectangle out of the lit image
  (`braille_graph::column_rect`: dot centres on half pixels, 3 px pitch, so a column is a
  whole-pixel band) and the peak dot's out of the peak image, straight into the strip's shm
  buffer. Per-dot cairo arcs would be thousands per frame; per-column cairo fills cost about
  4x as much as the copies.
- **Each graph is its own subsurface** (340x129), so the dropdown isn't repainted; a frame is
  committed at most once per tick and only when a dot changed. The dropdown's own paint
  composes the same frame under each strip, so `snapshot` PNGs show it.
- **Cost** (release): one playing card with the dropdown open 0.55 % CPU; closed 0.10 %.
- **The privacy indicator ignores it**: every capture node is named `stickBar-visualizer`
  (`graph::VISUALIZER_NODE_NAME`), which the privacy classification skips.
- Without PipeWire or `wl_subcompositor` the dropdown has no graph rows.

## Testing

- `tests/test_media.py` runs against `tests/lib/fake_mpris.py` (logs calls, never touches a
  real player): metadata, the chips, a middle click on the box (only when the fake is the only
  player playing, so the user's track is never skipped), seeking, shrinking when a player
  quits, and the idle box.
- **Tests never use real audio** (the user's rule): `test_visualizer.py` runs the bar with
  `STICKBAR_TEST_VISUALIZER_SYNTH=1`: no PipeWire, no capture; an active slot analyses sine
  tones set with `vis-tone <slot> <channel> <hz>`, and everything from there on is the real
  path. `STICKBAR_TEST_VISUALIZER_ONLY` limits it to the fake players. Every other test bar
  runs with the visualizer off (`STICKBAR_TEST_NO_VISUALIZER=1`, `tests/lib/common.py`).
- Not covered by tests, checked live instead: player-to-stream matching (Spotify by pid, the
  Helium browser through the ancestor walk), the capture itself (a fake player whose child
  plays 1 kHz left / 3 kHz right into a null sink: each tone only in its own half), the
  privacy filter, and the capture node going away when the dropdown closes or the player
  pauses.
