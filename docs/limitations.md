# Known limitations (not bugs)

Things the bar deliberately doesn't do, or doesn't do yet. Limitations explained in a
module's own doc aren't repeated here.

## Display and input

- **No HiDPI/fractional-scale handling.** `scale_factor_changed` in `src/wayland/mod.rs` is a
  no-op, so buffers are always 1x. Fine on this machine (both monitors are `scale: 1.00`).
- **Unplugging and replugging a monitor can shift workspace ranges**: `Output::index` is
  handed out in the order outputs appear and never reused, so it isn't tied to a stable
  monitor identity ([`architecture.md`](architecture.md)).
- **No keyboard handling** (no `wl_keyboard`, no libxkbcommon), so Escape closes nothing,
  unlike the reference power menu's `Keys.onEscapePressed`. Not judged worth a new
  subsystem: click outside, or use a power-menu trigger again.
- **No real `xdg_popup`.** Popups are their own `zwlr_layer_surface_v1`, placed with anchor
  and margin math, so there's no `xdg_wm_base`/`xdg_positioner` to implement.
- **Popups don't reflow at screen edges.** Right-anchored ones (tray, clock) are clamped at
  the output's left edge, not repositioned (also true of the reference); left-anchored
  ones aren't defended against overflowing the right edge, which no real caller is close
  enough to hit.

## Caps

Each is far above real use. Past one, the rest is cut off:

| What | Cap | Past it |
|---|---|---|
| DNS servers (`network::MAX_DNS`) | 8 | dropped (glibc only uses 3) |
| Spectrum graphs in the media dropdown (`visualizer::MAX_SOURCES`) | 4 | later cards get no graph; an app with several playback streams shows the running one |
| Smart plugs (`shelly::config::MAX_PLUGS`) | 12 | a settings-file error, not a silent drop |
| Update rows (`modules::updates::popup::MAX_ROWS`, AUR at most 10) | 30 | `+N more` |
| Proxmox nodes / guest rows / storage rows (`modules::proxmox::popup`) | 4 / 20 / 8 | `+N more` for rows; extra nodes aren't shown |
| EQ curve points (`eq::MAX_CURVE_POINTS`) | 128 | dropped |

Tray items and menu entries, audio devices, mixer streams, media players, PipeWire nodes,
privacy entries and CPU cores have no cap (`Vec`s and maps).

## Modules

- **Tray: PNG-only icon fallback, no SVG, no real freedesktop icon-theme lookup.** Items with
  no usable `IconPixmap` fall back to `dbus/icon_theme.rs`'s `lookup()`: the item's own
  `IconThemePath` first (a documented SNI property some apps, Steam among them, use for a
  private icon file), then a hand-rolled search of hicolor's common size directories and
  `/usr/share/pixmaps`. No `index.theme` parsing, no theme inheritance, no SVG (that would
  need librsvg). Items with neither get the muted placeholder square.
- **Tray: empty for the run if no StatusNotifierWatcher can be used** — neither our own
  (the name is taken) nor another one answering `RegisterStatusNotifierHost`. Logged to
  stderr ([`modules/tray.md`](modules/tray.md)).
- **Tray: no submenus.** `GetLayout` asks for depth 1, matching the reference's flat
  rendering. Submenu entries are marked with `›`, but clicking one only sends its own
  `clicked` event.
- **Tray: no tooltips** (the SNI `ToolTip` property is never fetched) **and no scroll-wheel
  `Scroll()`** (only volume and mic act on the wheel).
- **Volume/mic: no "no sound server" state.** If the pulse thread can't connect
  (`audio::start`), the boxes still draw at 0 % instead of hiding or showing an error.
  Only happens with PipeWire/PulseAudio missing entirely.
- **Mixer: no per-app output-device routing** (`move_sink_input_by_index` isn't wired up).
- **GPU stats are NVIDIA-only, via NVML.** No AMD or Intel equivalent, matching the
  reference `SystemMonitor.qml`. Only GPU index 0 is read.
- **Network: no VPN awareness.** The module shows whichever interface owns the default
  route, so a VPN with its own default route shows correctly, but nothing says it's a VPN.
- **Privacy: read-only.** No per-app mute or stop.
- **Power menu: no confirmation step.** A click runs `systemctl reboot`/`poweroff` at once,
  as in the reference.
