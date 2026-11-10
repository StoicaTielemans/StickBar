# Known limitations (first pass, not bugs)

Limitations explained in a module's own section aren't repeated here.

- **No HiDPI/fractional-scale handling.** `wl_output.scale` is received and
  discarded (`wayland/output.c`'s `handle_scale`) — fine on this machine (both
  monitors report `scale: 1.00`), but buffers are always allocated at 1x.
- **No live output reconfiguration.** A second `wl_output.done` burst after the
  first (e.g. a real resolution change) is detected and logged, but the bar
  surface isn't resized in response.
- **Output unplug/replug can shift workspace ranges**, since `Output.index` isn't
  tied to a stable monitor identity.
- **No pointer cursor image on hover** — clicks work, but the cursor glyph doesn't
  change when hovering the bar. `wayland-cursor` isn't even linked yet; add it
  back to `meson.build`'s dependencies when this is implemented.
- **No keyboard handling anywhere** (no `wl_keyboard` binding, no libxkbcommon
  dependency), so Escape closes nothing, unlike the reference power menu's
  `Keys.onEscapePressed`. Adding it purely for that wasn't judged worth the new
  subsystem: click outside, or use a power-menu trigger again.
- **No popups/tooltips backed by real `xdg_popup`** — popups are a second
  `zwlr_layer_surface_v1` positioned via anchor+margin math, a deliberate choice to
  avoid implementing `xdg_wm_base`/`xdg_positioner` from scratch.
- **Popups don't reflow at screen edges.** Right-anchored ones (tray, clock) are
  clamped at the output's left edge, not repositioned (also true of the reference);
  left-anchored ones aren't defended against overflowing the right edge, which no
  real caller today is close enough to hit.
- **Fixed-array caps** -- documented caps, not dynamic growth, comfortably above real
  use; past one, entries are silently dropped: tray items (`MAX_TRAY_ITEMS`), device
  popup rows (`MAX_DEVICE_ROWS`, 32), mixer streams (`MAX_PULSE_STREAMS`, 32), media
  players (`MAX_MPRIS_PLAYERS`, 16), DNS servers (`MAX_DNS_SERVERS`, 8, above the 3
  glibc's resolver honors), PipeWire nodes and links (`MAX_PW_NODES`/`MAX_PW_LINKS`,
  256 each; clients `MAX_PW_CLIENTS`, 128), privacy entries (`MAX_PRIVACY_ENTRIES`,
  32), media-dropdown spectrum graphs (`VISUALIZER_MAX_SOURCES`, 4: later cards get
  no graph; an app with several playback streams shows the running one). CPU cores
  (`MAX_CPU_CORES`, 128): past it, excess cores are dropped from the dropdown's
  per-core grid, but `/proc/stat`'s aggregate `cpu` line still sets the overall
  percentage.
- **Tray: PNG-only icon fallback, no SVG, no real freedesktop icon-theme spec
  compliance.** Items with no usable `IconPixmap` fall back to
  `dbus/icon_theme.c`'s `icon_theme_lookup()`: the item's own `IconThemePath`
  override first (a documented SNI property some apps, Steam among them, use to
  point at a private icon file — this is what fixed Steam's tray icon, which
  previously rendered a placeholder), then a pragmatic, hand-rolled search
  across hicolor's common size directories and `/usr/share/pixmaps` — **not**
  `index.theme` parsing, theme inheritance, or SVG (that would need
  librsvg/gdk-pixbuf, a new dependency this project doesn't have). Items with
  no `IconPixmap` *and* no PNG found by either path still render the muted
  placeholder square.
- **Tray: no fallback if stickBar's own Watcher registration itself fails**
  (e.g. `sd_bus_add_object_vtable()` erroring after the bus name was already
  claimed — logged to stderr, name released, falls back to Host mode) *and*
  no other Watcher happens to be running either — the tray then stays empty
  for the run.
- **Tray: no submenu/nested-menu support** — `GetLayout`'s recursion depth is
  capped at 1, matching the reference's own flat rendering. Submenu entries
  are marked with `›` but clicking one only sends its own `clicked` event.
- **Tray: an unresponsive tray app can freeze the bar for up to 25s** — the
  item's property fetch on registration (`sni_registry.c`'s GetAll) and the
  menu fetch on right-click (`dbusmenu.c`'s AboutToShow/GetLayout) are
  synchronous `sd_bus_call_method()`s with sd-bus's default 25s timeout,
  inside the single-threaded event loop. Never seen with a real app; found
  while writing `tests/lib/fake_tray.py`, whose first (blocking) version hung
  exactly this way. Fix would be async calls or a short timeout.
- **Tray: no tooltip support** (the SNI `ToolTip` property is never fetched), **and
  no scroll-wheel `Scroll()` support** (only the volume/mic modules act on
  `axis_discrete`).
- **Volume/mic: no visual "no PulseAudio-compatible server" state** — if
  `pulse_client_init()` fails, the volume/mic boxes still render (0%, not
  muted, since every accessor returns a safe default when `g_mainloop` is
  NULL) rather than being hidden or showing an explicit error, unlike the
  tray's `count == 0` → zero-width behavior. Low-risk in practice (this
  failure mode requires a broken/absent PipeWire or PulseAudio installation
  entirely) but worth knowing if it's ever actually hit.
- **Mixer: no per-app output-device routing** (that's
  `pa_context_move_sink_input_by_index`, not wired up).
- **GPU stats are NVIDIA-only, via NVML** — no AMD (`radeontop`/`rocm-smi`) or Intel
  (`intel_gpu_top`) equivalent, matching the reference `SystemMonitor.qml`'s own
  NVIDIA-only `Process` command. Only GPU index 0 is read.
- **Network: no VPN/tunnel-interface awareness** — this module (like the
  reference) only ever looks at whichever interface currently owns the
  default route; a VPN that adds its own default route makes this correctly
  show the VPN's own interface/IP, but there's no indication *that* it's a
  VPN specifically (no icon variant, no "via" annotation).
- **Privacy: no per-app mute/stop control** — the dropdown is read-only; there's no
  equivalent of PipeWire's own ability to suspend a node from this bar.
- **Power menu: Restart/Shutdown have no confirmation step** — a single click
  immediately runs `systemctl reboot`/`systemctl poweroff`, matching the reference
  exactly, so this is parity, not a regression, but worth knowing if a stray click
  is a real concern on a given setup.
