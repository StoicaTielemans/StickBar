# Popups (every dropdown, and what each mouse button does)

Shared by all of them; a module's own doc only records what differs.

| Box | Left | Right | Middle | Scroll | Dropdown anchor |
|---|---|---|---|---|---|
| workspaces | focus that workspace | | | | |
| CPU / GPU / memory | | dropdown | | | TOP+LEFT |
| Proxmox | | dropdown (Wake chip inside) | | | TOP+LEFT |
| privacy | | dropdown | | | TOP+LEFT |
| media | play/pause (nothing when idle) | dropdown: players, EQ, smart plugs | next track (nothing when idle) | | TOP, centred on the monitor |
| tray | see the has-menu rule in [modules/tray.md](modules/tray.md) | same | | | TOP+RIGHT |
| clock | | calendar (toggle) | | | TOP+RIGHT |
| network | | dropdown | | | TOP+RIGHT |
| volume | mute | device dropdown | mixer | ±5% | TOP+RIGHT |
| mic | mute | device dropdown | | ±5% | TOP+RIGHT |
| updates | | dropdown (package list) | | | TOP+RIGHT |
| Arch logo | | power menu | | | full-screen overlay |

Inside the media dropdown, a card's chips play/pause or skip, a click on its timeline seeks
([modules/media.md](modules/media.md)), an EQ profile chip applies that profile
([modules/eq.md](modules/eq.md)), and a plug's chip switches it
([modules/shelly.md](modules/shelly.md)). The EQ and the plugs have no box of their own.

## Behaviour

- **Right-click, because there's no hover-open.** The reference opens most of these on hover
  (with a debounce timer, or its tooltip). The bar does track the pointer (for hover colours
  and the cursor), but opening on hover would need hover-duration timers per box; right-click
  was free and is a conventional way to show a secondary action. Status-only boxes
  (CPU/GPU/memory, network, privacy, updates, Proxmox) have no obvious primary action, so
  left-click does nothing; `SystemMonitorWidget.qml` has no `onClicked` at all.
- **Hand cursor** over every bar box and dropdown target that reacts to a click (any
  button, so right-click-only boxes count), arrow elsewhere, catchers included
  (`wp_cursor_shape_v1`). Each module marks its boxes `clickable`, and each dropdown answers
  "is (x, y) a target" through `App::cursor_shape_at`. Guarded by `test_cursor_shape`.
- **Popups mostly don't coordinate.** Each kind is a singleton (at most one of a kind
  open), and opening one doesn't close another kind: right-click a tray icon, then the volume
  box, and both stay open. The exceptions hang off the same spot and close each other:
  CPU/GPU/memory, and the mixer with the sink dropdown. It matters most for the power menu,
  whose full-screen surface covers whatever else was left open without closing it.

## Look

A `mantle` surface with the module's accent as a `BAR.module_border_width` border, square
corners; a header (glyph, bold title, dim summary, and a filled status pill on the right,
with text that would run into the pill ellipsized); `base` cards with a 1px `surface1`
border, a 2px accent border marking the current item (active player, default device,
current month, active profile), hover lightening to `surface0`.

## Code

- `src/bar/popup_surface.rs`: `PopupSurface`, a menu or a catcher.
- `src/bar/popup_host.rs`: `PopupHost`, a menu and its catchers, placed under the box
  (`open_left`, `open_right`, `open_centered`) and resized (`resize`). A dropdown holds
  `Option<PopupHost>`; dropping it closes the menu and every catcher.
- `src/bar/popups.rs`: `Popups`, one field per dropdown kind.
- `src/render/popup_chrome.rs` (frame, header, card) with the tokens in
  `src/render/popup_style.rs`; `src/render/stat_card.rs` for the stat dropdowns' graph card,
  axis, rows and top-apps list. A popup that really differs keeps a local constant with a
  comment saying why.
- `src/wayland/pointer.rs`: seat, pointer, and routing clicks and hover.

## Rules

- **First paint:** a paint before the first configure only sets `needs_paint`, and
  `PopupSurface::configure` returns it so the owner paints again. Nothing keeps the drawn
  frame: the drawing goes into a temporary image that's freed after the copy into shm, so an
  open dropdown costs only its shm buffer. `snapshot` redraws into a fresh image, which is
  why `test_calendar` checks the first paint in the `WAYLAND_DEBUG` log, not a snapshot.
- **The top margin subtracts `BAR.height` from the trigger's bottom edge**, not just
  `box_y + box_h`. The layer-shell spec measures `set_margin` from the output edge, but the
  first popup rendered one bar-height too low without it. Working theory (from the spec
  text and the round size of the gap, not confirmed in Hyprland's source): Hyprland
  measures a layer surface's margin from the edge of the exclusive zone reserved by the same
  client, so the bar's own zone was being added twice.
- **Anchor by which side of the bar the trigger is on**: right-region popups grow leftward
  from the trigger's right edge, left-region ones rightward from its left edge. Media sits in
  the centre, so its dropdown is anchored TOP only, which layer-shell centres horizontally,
  so a width change stays centred too. Audio dropdowns anchor right where the reference
  left-aligns under the widget.
- **Dismissal: one full-screen, transparent, `LAYER_TOP` click-catcher per output**, not
  just the output the menu is on. The reference's `HyprlandFocusGrab` captures
  compositor-wide; with 2 monitors, a single-output catcher would leave a menu stuck open
  after a click on the other one. The menu itself is on `LAYER_OVERLAY`, strictly above the
  catchers, so it gets its own clicks first. A click on the menu's own surface is absorbed.
- **Catchers are a 1x1 buffer stretched with `wp_viewporter`**: `set_destination` on every
  configure, and the input region still covers the whole output. Without a viewporter, a
  full-size transparent buffer. SCTK has no viewporter helper: `src/wayland/viewporter.rs`
  binds it and implements SCTK's `Dispatch2` for it (it has no events).
- **Changing size while open means recreating the menu surface, not `set_size()`**:
  Hyprland unmaps a popup whose `set_size()` shrinks it. `PopupHost::resize` replaces the
  menu surface (the catchers stay), the same path as an open. The mixer, device, media,
  updates and Proxmox dropdowns do this when their rows change; the rest are a fixed size
  while open.
- **Animation goes on a subsurface, not into the popup**: the media dropdown's spectrum is a
  `bar/strip_surface.rs` child of the popup surface, so the popup itself only repaints on
  discrete changes. When a popup surface is recreated, its strips must be destroyed first
  and recreated on the new one.
- **Live vs. snapshot**: audio and media re-read their always-current cache on every update
  while open, with fixed content widths (340 px audio, 360 px media per column) so only the
  height changes. The tray (a one-shot D-Bus fetch), calendar and privacy (re-snapshotted on
  each right-click) snapshot at open. CPU/GPU/memory/network repaint on the 5 s poll
  (`refresh_if_open`); updates, Proxmox and the media dropdown's cards when their data
  arrives (`refresh`).
- **Graph width (network, CPU, GPU, memory)**: as many cells as the card holds
  (`braille_graph::cells_for_width`); histories hold 120 samples (`util::history::HISTORY_LEN`,
  10 min at one per 5 s poll) so a wide card can fill; peaks are taken over the visible
  samples only, and the axis label comes from the real span (`braille_graph::span_label`,
  e.g. `8 min ago`). 30 cells is each card's minimum width.
