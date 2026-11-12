# Privacy indicator (mic/webcam/screen-share)

Has no equivalent in the reference Quickshell bar at all — this is a waybar-style
addition, added per explicit user request, showing when any application is
recording the mic, reading the webcam, or capturing the screen, and which
application. Structurally the newest "domain" subsystem alongside `dbus/`,
`audio/`, `sysmon/`, `mpris/`, and `network/`: `pipewire/pipewire_client.c` talks
directly to PipeWire (not via the PulseAudio-compatible protocol
`audio/pulse_client.c` uses for volume/mic control — that protocol has no
equivalent visibility into the raw stream graph or into video streams at all),
and `modules/privacy/privacy.c` + `privacy_info_popup.c` are the bar-facing half.

- **`pipewire_client.c` reuses `pulse_client.c`'s exact shape**: a `pw_thread_loop`
  (PipeWire's own tested analogue of `pa_threaded_mainloop` — an owned background
  thread + lock, chosen over hand-rolling a `pw_loop`-in-epoll adapter for the
  same reasoning `pulse_client.c` already gives for its own choice), one eventfd
  for cross-thread change notification, and the thread-loop's own lock reused as
  a general-purpose mutex over two fixed-size tables (`g_nodes`, `g_links`).
- **Detection is a `pw_registry` global listener**, tracking every `Node` global's
  `media.class` and a resolved display name (`application.name` →
  `node.description` → `node.name` → `media.name` → `"Unknown app"` fallback
  chain), plus every `Link` global's `link.output.node`/`link.input.node` — the
  latter read straight off the link's own registry **global properties**, with
  no need to bind the link object at all (confirmed live via `pw-dump` during
  development: a link's `props` already carries both node ids). A node whose
  `media.class` is `Stream/Input/Audio` or `Stream/Input/Video` is additionally
  bound (`pw_registry_bind` + a `pw_node_events.info` listener) so its live
  `PW_NODE_STATE_RUNNING` state is tracked — a node merely *existing* isn't
  enough to count as "in use" (an app can hold the device open while muted/
  paused), matching this project's general instinct toward explicit-state-over-
  presence already seen in `pulse_client.c`'s own default-device tracking.
- **Mic detection is independently confirmed live**: a `parecord` capture during
  development produced exactly the expected node
  (`media.class=Stream/Input/Audio`, `state=running`,
  `application.name=parecord`), and the full pipeline (icon appears within one
  redraw, right-click dropdown lists `parecord` under MICROPHONE, icon disappears
  when the recording ends) was screenshot-verified end-to-end via `grim`, plus a
  real synthetic right-click via `ydotool`/`ydotoold` (this user has `/dev/uinput`
  access via the `input` group).
- **Webcam vs. screen-share is an elimination heuristic, NOT independently
  verified** — both show up identically as a `Stream/Input/Video` node with no
  tag on the node itself saying which. `video_node_is_webcam()` in
  `pipewire_client.c` follows the `Link` whose `link.input.node` is this stream
  back to its `link.output.node`, and checks *that* node's own `media.class`: a
  prefix of `Video/Source` (a real v4l2/libcamera device-monitor node) →
  webcam; anything else (including no link yet) → screen-share, by elimination.
  The `Video/Source` convention itself is high-confidence — it's PipeWire's own
  documented device-monitor role naming, and was independently confirmed live
  for the exactly analogous audio case (real ALSA mic devices show up as
  `Audio/Source` nodes, confirmed via `pw-dump` while `parecord` was running) —
  but this machine has no `/dev/video*` device and driving
  `xdg-desktop-portal-hyprland`'s screen-share picker needs interactive UI a
  headless test can't provide, so the actual webcam/screen-share split was
  never exercised against real hardware/a real portal session. **If this is
  ever wrong in practice, `video_node_is_webcam()` is the one function to
  re-check first** — most likely fix would be adjusting which `media.class`
  prefix (or which other property) actually distinguishes a portal-created
  screen-capture producer on the machine where it's wrong.
- **The bar's own media visualizer is skipped** (matched by `node.name` ==
  `VISUALIZER_NODE_NAME`, shared by all its capture streams): each graph in the
  media dropdown captures a player's playback stream as a running
  `Stream/Input/Audio` node, which would otherwise show a mic icon naming
  stickBar whenever the dropdown is open during playback. See [media.md](media.md).
- **`pipewire_client_pump()` computes the three flags once per notification** (one
  locked scan) and caches them; the bar is redrawn only when they changed
  (`PIPEWIRE_PRIVACY_CHANGED`), and `privacy.c` reads the cache instead of three
  locked scans per frame. Most graph changes are sinks, filters, links or the
  visualizer's own captures: measured, 20 null-sink add/removes cost 130ms of CPU
  before, 10ms after.
- **Playback streams (`Stream/Output/Audio`) and Client objects are tracked too**,
  only for the media visualizer's player <-> stream matching
  (`pipewire_find_playback_for_pid()`); every privacy accessor matches
  `Stream/Input/*` by name, so they never count here.
- **Accent color is `COLORS.maroon`** — confirmed via a full-codebase grep that
  `rosewater`, `flamingo`, `maroon`, and `lavender` were the only Catppuccin
  Mocha palette entries no other module had claimed yet at the time this was
  added; maroon was picked for its "recording/alert" connotation, and doesn't
  collide with `red` (already the power menu's Shutdown-button color).
- **New glyphs** (`config/glyphs.h`): `GLYPH_ICON_WEBCAM` (`0xF05A0`,
  `nf-md-webcam`) and `GLYPH_ICON_SCREENSHARE` (`0xF1483`, `nf-md-monitor_share`),
  both found via a live fontTools cmap search against
  `JetBrainsMonoNerdFont-Regular.ttf` for glyph names containing
  "webcam"/"monitor_share" (same verification convention as every other glyph in
  this file — not guessed from a cheat sheet). Mic reuses the existing
  `GLYPH_ICON_MIC` the volume/mic modules already use.
- **The bar widget (`modules/privacy/privacy.c`) lives in the bar's left region**,
  right after the memory module, and — per explicit user request ("when I don't
  use mic or screenshare I just don't want to see anything") — **collapses to
  zero width when nothing is active**, the same convention
  `modules/media/media.c` uses for "no MPRIS player at all": no box is drawn and
  the render function returns its input `x` unadvanced, which render.c's plain
  `x = module_render(cr, output, x, ...)` chaining tolerates with no special
  case needed. When at least one category is active, the box shows one icon per
  active category (mic/webcam/screen-share, space-joined), all in maroon, no
  percent/text unlike cpu/gpu/mem's own boxes.
- **Dropdown contents** (it used to be one plain maroon text block): a shield
  glyph, `Privacy · N streams` and a maroon
  `in use` pill, then one `base` card per active category in MIC/WEBCAM/SCREEN
  SHARE order (a card is omitted if empty), with the category's icon, title and
  count on top and one app name per line. The width follows the longest name,
  260-480px (past that it's clipped), since the snapshot can't change size
  while it's shown. `tests/test_privacy.py` drives it with a real `parecord` on
  a throwaway null sink's monitor, so no real mic is opened.

## In the Rust port

- **Files:** `src/pipewire/mod.rs` (the `pipewire` thread: registry and node listeners),
  `src/pipewire/graph.rs` (the nodes and links, and the classification, with unit tests and no
  PipeWire types), `src/modules/privacy/mod.rs` (the box), `src/modules/privacy/popup.rs`
  (`PrivacyPopup`). pipewire-rs 0.10 (bindgen at build time: needs libclang and the PipeWire
  headers).
- **A `pipewire` thread owns the connection** (the C bar's `pw_thread_loop`). Its graph is an
  `Rc<RefCell<..>>`: pipewire-rs gives every event its own `'static` closure on that one loop,
  so a `RefCell` is all the sharing needs. Node listeners hold a `Weak`.
- **The thread sends who's recording what, and only when it changed** (C's cached pump):
  measured, 20 null-sink add/removes cost the bar 0 commits and under 10 ms of CPU (C: 10 ms
  after its fix). The bars redraw only when one of the three kinds turns on or off;
  `App::privacy_update` keeps the list for the dropdown, which snapshots it at open.
- **Order within a card:** PipeWire node id (roughly creation order). C used its node table's
  slot order, so with several streams in one card the two bars can list them differently;
  everything else is pixel-identical.
- **Playback streams and clients** are tracked too (bound for their running state and
  `application.process.id`), only for the visualizer's player-to-stream matching
  (`Graph::find_playback`, with `capture::descends`). The privacy state only looks at
  `Stream/Input/*`, so they never count here.
- **No fixed caps:** nodes, links and entries are `HashMap`s, `BTreeMap`s and `Vec`s (C: 256 /
  256 / 32).
- The webcam / screen-share split is ported unchanged and is as unverified as in C (no camera,
  no portal session on the development machine).
