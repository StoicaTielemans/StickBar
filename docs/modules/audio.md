# PulseAudio / volume & mic

Files: `src/audio/mod.rs` (`AudioState` snapshot, `Command`, the `Audio` handle, percent
maths), `src/audio/pulse.rs` (the pulse thread), `src/audio/device_popup.rs`,
`src/audio/mixer_popup.rs`, `src/audio/volume_style.rs`, `src/modules/{volume,mic}/mod.rs`.

The volume (default sink) and mic (default source) boxes talk to `pipewire-pulse`
(`pactl info`: `Server Name: PulseAudio (on PipeWire 1.6.8)`) through **libpulse**
(`libpulse-binding`), not the native PipeWire API. Getting "the sinks with a description and
volume" from libpulse's introspection API is far less ceremony than PipeWire's
node/param negotiation, and `pipewire-pulse` provides it on any PipeWire system (waybar's
`pulseaudio` module does the same).

## The pulse thread

- **One thread owns everything, channels instead of a lock.** The binding's types aren't
  `Send`, so the `pulse` thread creates a standard mainloop and the context and never shares
  them; its cache is an `Rc<RefCell<..>>` only its own callbacks touch. It loops
  `iterate(true)` → run queued commands → if anything changed, send one full `AudioState` to
  the main thread on a calloop channel (one per iteration, so N sinks are one update, not N).
  Commands go the other way on an `mpsc` channel plus one byte on a socket pair, whose read
  end is a libpulse IO event, so the blocked `iterate` wakes. Volume steps are computed on the
  pulse thread from its cache.
- **Startup waits at most 2 s** for the connection (`CONNECT_TIMEOUT`), then reports failure
  once and the boxes show `0%` ([limitations.md](../limitations.md)).
- **Redraw only when what the bar shows changed:** `App::audio_update` redraws the bars only
  when the default sink's or source's (volume, mute) changed (`AudioState::shown`). Most
  notifications are other apps' streams, non-default devices or a sink going idle.
  `test_redraw`'s no-op check covers it. Open dropdowns refresh on every update.
- **The cache** keeps sinks, sources and streams by PA index (`Vec`s, no cap). The default
  sink/source are matched by name (`pa_server_info`'s `default_sink_name`/`_source_name`),
  re-fetched on a `SERVER` subscription event, which fires when the default changes.
- **Volume percent**: average over channels × 100 / `PA_VOLUME_NORM` (65536 = 100 %), and the
  reverse for setting: a uniform volume over the device's own channel count (2 if never
  seen). `MAX_VOLUME` is 200 and `VOLUME_STEP` 5, matching the reference
  `AudioMonitor.qml`'s `maxVolume: 2.0` / `step: 0.05`.

## The boxes

Left click mutes, right click opens the device dropdown, scroll steps ±5 %, and a middle click
on the volume box opens the mixer ([popups.md](../popups.md)).

- **Scrolling** reads `value120` (SCTK binds `wl_seat` up to v10): partial high-resolution
  movement is added up and one ±5 % step is taken per full 120 (one notch); `discrete` is the
  fallback for older seats; touchpad-only scrolling is ignored. Up is louder.

## Device dropdown (`device_popup.rs`)

- **One dropdown for both sides** (`Kind::Sink` / `Kind::Source`). **The per-row status icon
  differs on purpose**, as in the reference's two different `muteIcon(node)`: sinks get the
  4-level muted/low/mid/high icon, sources the 2-state mic icon (`status_glyph(kind, ..)`).
- **Cards**: name (accent + `default` tag on the default device, whose card also gets the 2px
  accent border; `set default` on hover for the others), a detail line
  `USB · 48 kHz · 2 ch · running` (bus from `device.bus`, rate, channels, state: running green,
  idle yellow, suspended dim), a `meter_bar` volume meter, the percent and a mute chip. Names
  that don't fit are cut at a character boundary (`TextCache::truncate`), the popup doesn't
  grow. The header pill shows the default device's volume or `muted`.
- **Volume is adjustable** (the user asked for it): a press on the meter sets it and starts a
  drag, scroll anywhere on a card steps it. The meter spans 0-100 % like the mixer's slider;
  above that (scroll goes to 200 %) it's full with a peach top end. A drag remembers the last
  percent sent and draws that instead of the server's figure until the release.
- **Monitor sources are hidden from the mic list** (`node.monitor`): every sink's `.monitor`
  used to be listed, outnumbering the real inputs three to two. A monitor that *is* the
  default stays visible so it can be switched away from.
- The popup returns `Command`s and `App` sends them, so it never touches the pulse thread.
  Cards hold the device's index and look it up in the latest snapshot on every paint. A
  change in the number of cards resizes the dropdown ([popups.md](../popups.md)).

## Mixer (`mixer_popup.rs`)

Middle-click on the volume box (no reference equivalent; added at the user's request). One
card per app playback stream (PA sink input), named by `application.name` with the stream's
own name as fallback: app name, a full-width segmented 0–100 % slider (click to jump, drag to
adjust, scroll for ±5 %, which can still go past 100 % up to 200 %), the percent and a mute
chip. Header: `Mixer · N apps · <default sink>` and the same pill. An empty-state card means
it always opens. Recording apps (source outputs) aren't listed. Streams without
`has_volume` (passthrough) get no volume requests.

- **Drag relies on Wayland's implicit pointer grab**: while the button is held, motion keeps
  arriving on the mixer surface even outside it, and the button release ends the drag. The
  mixer's surfaces are routed before everything else.
- Apps start and stop streams far more often than devices appear, so this dropdown resizes
  often. A drag in progress loses its grab across the resize; the next release still ends it.

## Testing

`tests/test_audio.py`, through the test-control channel, on a throwaway null sink: meter
press/drag/scroll and the mute chip, the mic list without monitors, the mixer growing and
shrinking with a `paplay` stream, its slider and mute chip. Never a real device's volume or
the default device: setting a default by clicking a card isn't tested (it would reroute the
user's audio).
