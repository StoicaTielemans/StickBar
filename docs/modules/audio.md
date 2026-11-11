# PulseAudio / volume & mic

The volume (default sink) and mic (default source) modules talk to
`pipewire-pulse` — PipeWire's PulseAudio-protocol-compatible server, which is
what's actually running on this machine (`pactl info` reports `Server Name:
PulseAudio (on PipeWire 1.6.8)`) — via **libpulse**, not raw `libpipewire`.

- **Why libpulse instead of the native PipeWire client API**: `libpipewire`'s
  own client API is asynchronous node/param-negotiation machinery designed for
  real audio graph participants (media players, effects); getting a "list of
  sinks with a description and volume" out of it is substantially more
  ceremony than the same query via libpulse's introspection API
  (`pa_context_get_sink_info_list()` and friends), and `pipewire-pulse` makes
  that simpler API available for free on any system already running PipeWire
  (which is effectively every current desktop, this one included). This is
  also the same approach other minimal C status bars (e.g. waybar's
  `pulseaudio` module) take for the identical reason — not a novel choice.
- **`audio/pulse_client.c` runs libpulse's own `pa_threaded_mainloop`**, a
  background thread libpulse provides specifically for embedding into an app
  that doesn't want to hand control of its main thread to `pa_mainloop`'s
  blocking `iterate()` loop — a deliberate deviation from every other
  integration in this project (Wayland, Hyprland, D-Bus, the clock timer),
  which all instead implement a "give me your fds, I'll drive you from my own
  external epoll loop" adapter with no extra thread. That pattern *is*
  possible for libpulse too (`pa_mainloop_api` is an abstract vtable a caller
  can implement itself, exactly like this project already does implicitly for
  Wayland's `prepare_read`/`read_events` dance and explicitly for sd-bus's
  fd+timerfd integration in `dbus/dbus_loop.c`) — but hand-rolling one
  correctly means implementing arbitrary-count I/O watches, arbitrary-count
  timers, and "deferred" (run-every-iteration-until-disabled) callbacks from
  scratch, with subtle failure modes if any of the three is wrong. Given
  `pa_threaded_mainloop` is libpulse's own tested, correct implementation of
  exactly that adapter, reusing it was judged the better trade for this
  module specifically, at the cost of being the one part of this codebase
  that isn't single-threaded. All cross-thread interaction is confined to
  `audio/pulse_client.c` — every other file, including `audio/device_popup.c`
  and the volume/mic modules, only ever calls its plain synchronous-looking
  accessor functions, with no thread-safety concerns of their own.
- **Locking**: `pa_threaded_mainloop_lock()`/`_unlock()` (the mainloop's own
  mutex) is reused as a general-purpose lock for the whole local sink/source
  cache too, not just for calls into libpulse — every accessor in
  `pulse_client.c` (even a plain read like `pulse_node_muted()`) takes this
  lock. This is safe and sufficient because PA callbacks (which mutate the
  cache) are documented to always run on the mainloop thread *with this same
  lock already held*, so there's exactly one lock protecting exactly one
  piece of shared state, not two subsystems needing separate synchronization.
- **Startup blocks until the connection is definitively ready or definitively
  failed** (`pulse_client_init()`'s `pa_threaded_mainloop_wait()` loop) —
  libpulse's own documented pattern for synchronizing an owning thread against
  a `pa_threaded_mainloop`, and the same tradeoff `wayland_connect()` already
  makes with its two blocking `wl_display_roundtrip()` calls: better to know
  immediately (and log) that volume/mic controls are unavailable than to
  discover it several seconds later when no redraw ever arrives.
- **Change notification is a single eventfd** (`pulse_client_notify_fd()`),
  written from inside a PA callback (safe without the mainloop lock — it
  doesn't touch any of libpulse's own data structures) any time the cache
  changes, and drained by `pulse_client_pump()` on the *main* epoll loop
  (`util/notify_fd.c`). The pump reports whether what the bar shows -- the default
  sink's and source's volume and mute -- changed; only then is the bar redrawn
  (the open dropdowns refresh on every pump). Most notifications are other apps'
  streams, non-default devices or a sink going idle: measured, 300 no-op volume
  events cost the bar 660ms of CPU before and 30ms after.
- **The cache mirrors sinks/sources by stable `name`, not by PA `index`**,
  for matching against `pa_server_info.default_sink_name`/
  `default_source_name` — PA's numeric `index` isn't guaranteed stable across
  a device's full lifetime the way its string `name` is (this is also exactly
  why `sni_watcher.c`'s SNI item tracking keys off `(bus_name, path)` strings
  rather than any numeric id). `PA_SUBSCRIPTION_EVENT_SERVER` (fired when the
  *default* device changes, not when a device's own properties change) is
  what triggers a `pa_context_get_server_info()` re-fetch of those two names.
- **Volume percent conversion**: `pa_cvolume_avg(&volume) * 100 / PA_VOLUME_NORM`
  (`PA_VOLUME_NORM` = 65536 = 100%), and the reverse for setting — a uniform
  per-channel volume is written back via `pa_cvolume_set(&cv, channels, vol)`
  using the channel count cached from that device's own last-seen
  `pa_cvolume`, not hardcoded to 2 (stereo), though 2 is used as a fallback if
  a device's channel count was never seen yet. `PULSE_MAX_VOLUME_PERCENT` is
  200 (not 100) and `PULSE_VOLUME_STEP_PERCENT` is 5, both matching the
  reference `AudioMonitor.qml`'s own `maxVolume: 2.0` / `step: 0.05` exactly.
- **`audio/device_popup.c` is one shared singleton for both the sink and
  source dropdowns** (`is_sink` selects which).
  - **Each popup kind's per-row status icon deliberately differs**, matching
    the reference's own two *different* `muteIcon(node)` functions rather than
    reusing one: `AudioDevicesPopup.qml`'s is the same 4-level
    muted/low/mid/high icon as `VolumeWidget.qml`'s own; `MicDevicesPopup.qml`'s
    is the simple 2-state mic icon. `device_popup.c`'s
    `status_icon_glyph()` branches on `is_sink` for exactly
    this reason — not an oversight if the two ever look asymmetric in a diff.
  - **Description truncation is byte-based, not UTF-8-codepoint-aware**
    (`truncate_to_width()`) — deliberately, not an oversight: every real
    device description seen from this project's own actual hardware during
    development (e.g. `"Burr-Brown from TI USB Audio CODEC Analog Stereo"`,
    `"Yeti X Analog Stereo"`) is plain ASCII, matching how PulseAudio/PipeWire
    device descriptions are conventionally generated from ALSA card/device
    names in practice.
- **Dropdown contents** (`device_popup.c`, `mixer_popup.c`; names truncate instead
  of resizing the popup):
  - **Device cards**: name (accent + `default` tag on the default device,
    whose card also gets the 2px accent border; `set default` on hover for
    the others), a detail line `USB · 48 kHz · 2 ch · running` (bus from the
    `device.bus` proplist, `sample_spec.rate`, channels, and the sink/source
    state -- running green, idle yellow, suspended dim; `pa_sink_state_t` and
    `pa_source_state_t` share values so one mapping serves both), then a
    `meter_bar` volume meter, the percent and a mute chip. The header pill
    shows the default device's volume or `muted`.
  - **Device volume is now adjustable** (per explicit user request): press
    on the meter sets it and starts a drag, scroll anywhere on a card steps
    it by `PULSE_VOLUME_STEP_PERCENT` (`pulse_node_set_volume_percent()` /
    `_adjust_volume()`, by index, with the same stale-pointer `in_use` check
    as the stream setter). The meter spans 0-100% like the mixer's slider;
    above that (scroll still goes to 200%) it's full with a peach top end.
    `device_popup_handle_click()` became `_handle_press()` + `_release()` +
    `_scroll()`, routed from `pointer.c` next to the mixer's.
  - **Monitor sources are hidden from the mic list** (`monitor_of_sink !=
    PA_INVALID_INDEX`, filtered in the popup, not in `pulse_source_count()`):
    every sink's `.monitor` used to be listed, outnumbering the real inputs
    on this machine three to two. A monitor that *is* the default stays
    visible so it can be switched away from.
  - Mixer: header `Mixer · N apps · <default sink>` + the same pill.
  - Verified by `tests/test_audio.py` through the test-control channel (the
    user was in a fullscreen game, so no ydotool): on a throwaway null sink,
    meter press/drag/scroll and the mute chip; the mic list without
    monitors; the mixer growing/shrinking with a `paplay` stream and its
    slider/mute chip. The device cards were also checked in a `snapshot`
    PNG. Setting a default by clicking a card isn't exercised (it would
    reroute the user's audio).
- **Middle-click on the volume box opens a per-application mixer**
  (`audio/mixer_popup.c`, no reference-bar equivalent — added per explicit
  user request). `pulse_client.c` additionally tracks PA *sink inputs* (one
  per app playback stream, `PA_SUBSCRIPTION_MASK_SINK_INPUT`) in a third
  fixed array (`MAX_PULSE_STREAMS` = 32), named by `application.name` with
  the stream's own name as fallback. One card per stream: app name, a full-width
  segmented 0–100% slider (click to jump, drag to adjust, scroll for ±5% — scroll
  can still push past 100% up to the shared 200% cap), the percent, and a mute chip. Mic-side source outputs (apps *recording*) aren't listed.
  - **Drag relies on Wayland's implicit pointer grab**: while the button is
    held, motion keeps arriving on the mixer surface even outside it, so
    `wayland/pointer.c` routes button *releases* (previously ignored
    entirely) to `mixer_popup_handle_release()` to end the drag. Middle-click
    is filtered in `pointer_button()` to reach only `volume_handle_click()` —
    several other handlers treat "any non-left button" as their secondary
    action (the tray's has-menu rule) and would misfire otherwise.
  - Apps start/stop streams far more often than devices appear, so this popup
    resizes often (see [popups.md](../popups.md)). An in-progress drag loses its grab across a
    recreate; the next release still ends it.
- **Scroll direction convention** (`wayland/pointer.c`'s
  `pointer_axis_discrete()`): scrolling up decreases the wheel's `discrete`
  value below zero and increases volume; scrolling down does the reverse —
  reasoned from `wayland.xml`'s doc comment (axis `value`/`discrete` share "a
  coordinate space identical to those of motion events," i.e. positive = the
  same direction as scrolling a page down) plus the conventional Linux
  volume-scroll mapping, **not independently verified against a real mouse**.
  Trivial one-line flip (swap the ternary in `pointer_axis_discrete()`) if it turns
  out backwards in practice.
- **`wl_pointer`'s `axis_discrete` event (not the newer, higher-resolution
  `axis_value120`) drives scroll-to-adjust** — `axis_value120` was added in
  wl_pointer version 8, but `wayland/registry.c`'s `SEAT_VERSION` is 7 (the
  pointer object is only ever created at the bound seat's version), so
  `axis_value120` is simply never sent to this process regardless of the
  compositor's own capabilities. `axis_discrete` (added in v5) is what's
  actually available, and its magnitude is deliberately ignored (treated as
  ±1 regardless of the reported value) — one call is meant to represent
  exactly one physical wheel notch, matching the "steps of 5%" behavior that
  was asked for.

## In the Rust port

- **Files:** `src/audio/mod.rs` (`AudioState` snapshot, `Command`, the `Audio` handle, percent
  maths), `src/audio/pulse.rs` (the pulse thread), `src/audio/volume_style.rs`,
  `src/modules/{volume,mic}/mod.rs`.
- **libpulse through `libpulse-binding`** (safe wrapper, same calls as C). The binding's
  types aren't `Send`, which is what pushed the design below.
- **One thread that owns everything, channels instead of a lock.** The `pulse` thread creates a
  *standard* mainloop and the context and never shares them; its cache is an `Rc<RefCell<..>>`
  only its own callbacks touch. It loops `iterate(true)` → run queued commands → if anything
  changed, send one full `AudioState` to the main thread on a calloop channel (one per iteration,
  so a list of N sinks is one update, not N). Commands go the other way on an `mpsc` channel plus
  one byte on a socket pair, whose read end is a libpulse IO event so the blocked `iterate`
  wakes. Volume steps are computed on the pulse thread from its cache, like C.
- **Redraw rule kept:** `App::audio_update` redraws the bars only when the default sink's or
  source's (volume, mute) changed (`AudioState::shown`); `test_redraw`'s no-op check covers it.
- **Startup** waits at most 2 s for the connection (C waits unbounded), then reports failure
  once and the boxes show `0%`.
- **No fixed caps:** devices and streams are `Vec`s (C's 32-slot arrays).
- **Scrolling** reads `value120` (SCTK binds `wl_seat` up to v10; C binds v7 and gets
  `discrete`). Partial high-resolution movement is added up and one ±5 % step is taken per full
  120; `discrete` is the fallback for old seats. Direction as in C (up = louder).
- **Device dropdown** (`src/audio/device_popup.rs`): constants, layout and hit areas ported 1:1
  (the percent column counts as the card body, as in C). The popup returns `Command`s and
  `App` sends them, so it never touches the pulse thread itself. Cards hold the device's index
  and look it up in the latest snapshot on every paint. A drag remembers the last percent
  sent, drawn instead of the server's figure until the release. Count changes resize through
  `PopupHost::resize`, which recreates the menu surface (C's rule) and keeps the catchers.
  The hand cursor shows over the cards (Rust-only, like the rest of the cursor shapes).
- **Mixer** (`src/audio/mixer_popup.rs`): the same shape as the device dropdown (rows by stream
  index, drag shows the last percent sent, `PopupHost::resize` on a row-count change, an
  empty-state card so it always opens). Volume requests skip streams without `has_volume`
  (passthrough), as in C. Its surfaces are routed before everything else, as C's are.
- **Middle button** only reaches the volume box (the mixer); other buttons than left/right/middle
  are ignored, as in C.
