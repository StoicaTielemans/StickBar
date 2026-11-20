# Privacy indicator (mic/webcam/screen-share)

Files: `src/pipewire/mod.rs` (the `pipewire` thread: registry and node listeners),
`src/pipewire/graph.rs` (nodes, links, clients and the classification: pure, unit-tested,
no PipeWire types), `src/modules/privacy/mod.rs` (the box), `src/modules/privacy/popup.rs`
(`PrivacyPopup`). pipewire-rs 0.10 (bindgen at build time: needs libclang and the PipeWire
headers).

No reference equivalent: a waybar-style addition at the user's request, showing when any app
records the mic, reads the webcam or captures the screen, and which app. It talks to PipeWire
itself, not through the PulseAudio protocol the volume boxes use: that protocol can't see the
stream graph or video at all.

## Detection

- **A `pipewire` thread owns the connection.** Its graph is an `Rc<RefCell<..>>`:
  pipewire-rs gives every event its own `'static` closure on that one loop, so a `RefCell` is
  all the sharing needs. Node listeners hold a `Weak`. The thread also runs the media
  visualizer's capture streams ([media.md](media.md)).
- **A registry listener** tracks every `Node`'s `media.class` and a display name
  (`application.name` → `node.description` → `node.name` → `media.name` → `"Unknown app"`),
  and every `Link`'s `link.output.node`/`link.input.node`, read straight off the link's
  registry global properties (no bind needed; checked with `pw-dump`).
- **Running, not just existing:** `Stream/Input/Audio` and `Stream/Input/Video` nodes are
  bound and their state followed; only a running node counts (an app can hold a device open
  while paused).
- **Webcam vs. screen-share is an elimination heuristic, not verified** (`Graph::is_webcam`):
  both are a `Stream/Input/Video` node with nothing on the node saying which. The link into
  the stream is followed back to its source node: a `media.class` starting with
  `Video/Source` (a real v4l2/libcamera device) means webcam, anything else (or no link yet)
  screen-share. The `Video/Source` naming is PipeWire's device-monitor convention (the audio
  equivalent, `Audio/Source`, was confirmed live), but this machine has no camera and no
  portal session to test with. **If it's ever wrong, `is_webcam` is the place to look.**
- **The bar's own visualizer is skipped**: its capture streams are running
  `Stream/Input/Audio` nodes named `stickBar-visualizer`, which would otherwise show a mic
  icon naming stickBar whenever the media dropdown is open during playback.
- **Playback streams and clients are tracked too**, only for the visualizer's
  player-to-stream matching (`Graph::find_playback`); the privacy state only looks at
  `Stream/Input/*`.
- **The thread sends who's recording what, and only when it changed.** The bars redraw only
  when one of the three kinds turns on or off; `App::privacy_update` keeps the list for the
  dropdown. Most graph changes are sinks, filters, links or the visualizer's own captures:
  20 null-sink add/removes cost 0 commits and under 10 ms of CPU.

## The box and dropdown

- **In the left region, after memory and Proxmox, and only while something records** (the
  user: "when I don't use mic or screenshare I just don't want to see anything"):
  `privacy::render` returns `None` and nothing is drawn. Otherwise one icon per active kind
  (`ICON_MIC`, `ICON_WEBCAM` 0xF05A0, `ICON_SCREENSHARE` 0xF1483), all maroon, no text.
- **Accent `COLORS.maroon`**, for its "recording" feel, distinct from the power menu's red.
- **Dropdown** (right-click, snapshotted at open): a shield glyph, `Privacy · N streams` and
  a maroon `in use` pill, then one `base` card per active kind in MIC / WEBCAM / SCREEN SHARE
  order (empty ones left out), each with its icon, title and count, then one app per line in
  PipeWire node id order. The width follows the longest name, 260-480 px (clipped past that).
- **Testing**: `tests/test_privacy.py` records with a real `parecord` on a throwaway null
  sink's monitor, so no real mic is opened. Your own bar shows the indicator for those
  seconds too.
