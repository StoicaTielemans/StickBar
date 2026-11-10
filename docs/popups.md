# Popups (`bar/popup_surface.c` and every dropdown)

Shared by all of them; a module's own section only records what differs.

| Box | Left | Right | Middle | Scroll | Dropdown anchor |
|---|---|---|---|---|---|
| volume | mute | device dropdown | mixer | ±5% | TOP+RIGHT |
| mic | mute | device dropdown | | ±5% | TOP+RIGHT |
| CPU / GPU / memory | | dropdown | | | TOP+LEFT |
| privacy | | dropdown | | | TOP+LEFT |
| media | play/pause | dropdown | | | TOP+LEFT |
| network | | dropdown | | | TOP+RIGHT |
| clock | | calendar (toggle) | | | TOP+RIGHT |
| EQ | dropdown | dropdown | | | TOP+RIGHT |
| tray | see the has-menu rule in [modules/tray.md](modules/tray.md) | same | | | TOP+RIGHT |
| Arch logo | power menu | | | | full-screen overlay |

- **Right-click, because there's no hover**: the reference opens most of these on
  hover (with a debounce timer, or its tooltip). `wayland/pointer.c` only tracks
  which single surface the pointer is over, with no hover-duration or per-module
  enter/leave callbacks, and building that wasn't judged worth it when right-click
  was free and is a conventional way to expose a secondary action. Status-only
  widgets (CPU/GPU/memory, network, privacy) have no obvious "primary action", so
  left-click does nothing; `SystemMonitorWidget.qml` has no `onClicked` at all.
- **Look**: a `mantle` surface with the module's accent as a
  `BAR.module_border_width` border, square corners; a header (glyph, bold title,
  dim summary, and a filled status pill on the right); `base` cards with a 1px
  `surface1` border, a 2px accent border marking the current item (active player,
  default device, current month, active profile), hover lightening to `surface0`.
  This came from a restyling pass over every popup, per explicit user request, in
  this order: calendar, network, GPU, CPU, memory, media, audio/mixer, tray, then
  EQ, power menu and privacy.
- **Shared code, not copies**: the look lives in `render/popup_chrome.c`
  (`popup_draw_frame`, `popup_draw_header` -- which ellipsizes text that would run
  into the pill -- `popup_draw_card`) with the tokens in `render/popup_style.h`; the
  surface, its placement under the box, the catchers and open/close/resize in
  `bar/popup_host.c` (`PopupHost`); the stat dropdowns' graph card, axis, rows and
  top-apps list in `render/stat_card.c`. `wayland/pointer.c` routes clicks and hover
  through one table of popup kinds. Each of these used to be repeated in every
  dropdown (11 copies of the catcher code, 10 of the header and pill); a popup that
  really differs keeps a local `#define` with a comment saying why.
- **First paint**: `popup_surface.c` caches the last draw callback and self-triggers
  from its own `configure` handler (mirroring how `bar_surface.c`'s `configure`
  handler calls `bar_surface_mark_dirty()`), because the first
  `popup_surface_render_now()` runs before the first `configure` has been processed.
- **`top_margin` subtracts `BAR.height` from the trigger's bottom edge**, not just
  `box_y + box_h`. The layer-shell spec text describes `set_margin`'s distance as
  measured from the raw output edge, but the first popup (the tray's) rendered one
  full bar-height too low without the subtraction. The working theory (reasoned from
  the spec text plus the suspiciously round size of the gap, not confirmed against
  Hyprland's source) is that Hyprland measures a layer surface's margin from the
  edge of its *already-reserved exclusive-zone region* when the margin is set by the
  same client that owns that reservation, so the bar's own 20px zone was being added
  a second time.
- **Anchor by which side of the bar the trigger is on**: right-region popups grow
  leftward from the trigger's right edge, left-region ones rightward from its left
  edge (`left_margin = box_x`). Media sits in the center and
  was given TOP+LEFT as the more reasonable default. Audio dropdowns anchor right
  where the reference left-aligns under the widget, to reuse already-correct math.
- **Dismissal: one full-screen, fully transparent, `LAYER_TOP` click-catcher per
  currently-known output**, not just the output the menu is on. The reference's
  `HyprlandFocusGrab` captures compositor-wide; this machine has 2 monitors, so a
  single-output catcher would leave a menu stuck open after a click on the other
  one. The menu itself is on `LAYER_OVERLAY` (strictly above the catchers) so it
  reliably receives its own clicks first, without depending on same-layer
  creation-order stacking. A click on the menu's own surface is absorbed.
- **Catchers are backed by a 1x1 buffer stretched with `wp_viewporter`**
  (`popup_surface_create_catcher()`), not a full-output buffer: the surface size comes
  from `wp_viewport_set_destination()` (set from each `configure`), and the input region
  still covers the whole output. Falls back to a full-size buffer if the compositor has
  no `wp_viewporter` (bound optionally in `registry.c`).
- **Changing height while open means recreating the menu surface, not `set_size()`**:
  Hyprland unmaps a popup whose `zwlr_layer_surface_v1_set_size()` shrinks it.
  Destroy-and-recreate (`popup_host_resize()`) is the same path every popup open
  already uses. The mixer, device and media popups do this when their rows change;
  the rest are a fixed size while open.
- **Animation goes on a subsurface, not into the popup**: the media dropdown's
  spectrum is a `bar/strip_surface.c` child of the popup surface, so the popup
  itself still only repaints on discrete changes. When a popup surface is
  recreated (height change), its strip must be destroyed first and recreated on
  the new one.
- **Live vs. snapshot**: audio and media re-read their always-current local cache on
  every update while open (`*_refresh_if_open()`, a no-op when closed), with fixed
  content widths (340px audio, 360px media) so only the height can change. The tray
  (a one-shot D-Bus fetch), calendar (today's date) and privacy (which only changes
  on a real PipeWire graph change, and re-snapshots on each right-click) snapshot at
  open, since they're short-lived. CPU/GPU/memory/network repaint on the 5s poll; EQ on
  its state write.
- **Popups mostly don't coordinate.** Each kind is an independent singleton (at most
  one of a kind open), and opening one doesn't close another kind: e.g. right-click
  a tray icon, then the volume box, and both stay open. A rare, undefended v1 edge
  case. The exceptions hang off the same spot and close each other: CPU/GPU/memory,
  and the mixer with the sink dropdown. It matters most for the power menu, whose
  full-screen surface covers whatever else was left open without closing it.
- **Graph width (network, CPU, GPU, memory)**: `stat_graph_cells()` fits as many
  cells as the card holds (`braille_graph_cells_for_width()`), the histories hold 120
  samples (10 min at one per 5s poll) so a wide card can fill, peaks are taken over the
  visible samples only, and the axis label comes from the real span
  (`braille_graph_span_label()`: e.g. `8 min ago`). 30 cells stays as each card's
  minimum width.
