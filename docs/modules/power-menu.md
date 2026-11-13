# Power menu

The reference bar's logout/restart/shutdown overlay is its own second full shell
"part" (`powermenu/`, alongside `bar/`), triggered by `bar/modules/ArchLogoWidget.qml`
— the bar's own leftmost item, doing double duty as both a widget and a button —
toggling a shared `PowerMenuState` singleton. This project reproduces that same
split: `modules/powermenu/arch_logo.c` is the bar-surface trigger,
`modules/powermenu/power_menu.c` is the overlay itself, and both only ever talk to
each other through `power_menu_toggle()` — matching `PowerMenuState.toggle()`
exactly, with no separate `_open()`/`_close()` pair exposed, since every trigger
this bar has (the button, and the keybind below) always wants toggle semantics.

**Rust bar: the Arch logo opens it on right-click**, not left (user request, after the
port), like every other box's menu. A left click does nothing. `test_power_menu`
checks both.

- **`ArchLogoWidget.qml`'s "cutout" styling is deliberately *not* routed through
  the usual bordered module-box look** every other widget uses: a solid
  `COLORS.sapphire` fill with the Arch glyph rendered in `COLORS.mantle` (the
  bar's own background), no border at all (`module_box_draw_rect()` called with
  `border_width = 0.0`) — so the logo reads as a shape cut out of a solid block,
  not colored text on a dark box. `sapphire` and `red` (used below for the
  Shutdown button) were both still fully unclaimed by any other module's own
  accent color at the time this was added — unlike the network module's own
  forced `teal`-instead-of-`green` substitution (see [network.md](network.md)),
  no collision to work around here.
- **The Arch logo's box is sized from the glyph's *ink* extent**
  (`module_box_measure_text_ink()`) with the standard `+12px` pad, not the reference's
  `implicitWidth: label.implicitWidth + 8`: this glyph's ink is much wider than its
  logical width. `module_box_draw_text_centered()`'s own internal centering still uses
  the *logical* width (via `module_box_measure_text`), which is fine — the wider box
  gives it enough slack on both sides regardless.
- **One full-screen `zwlr_layer_surface_v1` per currently-known output**
  (`LAYER_OVERLAY`, all four edges anchored, no margin, matching the reference's
  own `Variants { model: Quickshell.screens }` — each screen gets its own
  `PanelWindow` with its own centered button row, not one surface spanning a
  combined virtual desktop) — same `popup_surface_create(..., 0, 0, ...,
  ANCHOR_TOP|BOTTOM|LEFT|RIGHT, ...)` shape every other popup's *click-catcher*
  surfaces already use (see e.g. `modules/cpu/cpu_popup.c`'s `create_catchers()`),
  except this surface actually draws visible content instead of staying
  transparent. The panel is centered on the surface's real size (`popup_surface_size()`,
  known from its first `configure`), not `Output.height`: the surface stops at the bars'
  exclusive zones.
- **The overlay's own full-screen surface doubles as its own click-catcher** —
  the one place in this project where a popup doesn't need `tray_context_menu.c`'s
  usual separate transparent catcher-surface-underneath-a-smaller-menu shape,
  since the menu content already covers the entire output by itself.
  `power_menu_handle_click()` hit-tests the three cards directly; landing on
  one triggers that action, landing elsewhere on the panel is absorbed (like a
  click on any dropdown's own surface), landing on the dimmed background just
  closes the menu — both paths call `power_menu_toggle()`'s own internal
  `close_overlay()` unconditionally first, matching the reference's own
  `PowerMenuState.close()` called before dispatching whichever action fired.
- **Background dim is 30% alpha over `COLORS.crust`, painted with
  `CAIRO_OPERATOR_SOURCE`, not the default `OVER`**
  (`cairo_set_source_rgba(cr, COLORS.crust.r, ..., 0.302)`), matching the
  reference's own `"#4D" + Config.colors.crust.substring(1)` (`0x4D / 255 ≈
  0.302`). This surface's two ping-ponged shm buffers each still hold their own previous
  frame, so a translucent paint must replace the destination, not blend onto it. Any
  future translucent full-surface paint needs `SOURCE` too.
- **A panel on the dim** (it used to be three free-standing 150×150 bordered
  buttons), kept as an overlay because the keybind has no bar box to anchor a
  dropdown to: a fixed 388×168 panel, `mantle` with
  a `sapphire` border (the Arch logo box's colour); header with the power glyph,
  `Power · user@host` and an uptime pill (both read once at open; no pill if
  `/proc/uptime` can't be read); three `base` cards in a row, each with its icon
  (`FONT.display`, a 32px size tier that exists for this -- the reference's
  `config.js` uses 40), name and a one-line description. On hover a card
  lightens and its border and name take the action's colour (yellow/sky/red).
- **A test-control instance can't run the actions**: `power_menu_handle_click()`
  records the card's action (`menu`'s `power_last_action`) and returns, so tests
  can click all three cards. `tests/test_power_menu.py` adds a second layer: that
  instance has a fake `systemctl` first on its `PATH` and a Hyprland signature
  matching no socket, and the test fails if the fake ever ran. The real
  logout/reboot/poweroff calls therefore aren't covered by any test. **Don't
  remove the guard to test something**, not even temporarily, on a session
  someone is using.
- **Explicit per-button code (Logout/Restart/Shutdown), not a data-driven
  table** — a `static const` table can't hold `COLORS.*` values at all (`COLORS`
  is `extern const`, not a compile-time constant, so referencing it inside a
  static initializer is a compile error), and three buttons is small enough
  that explicit code reads at least as clearly as a table would anyway,
  matching this project's existing preference for a flat list over indirection
  at this kind of N (`wayland/pointer.c`'s own click-routing list is the same
  shape).
- **Actions**: Logout sends `dispatch exit` over Hyprland's request socket
  (`hyprland_dispatch_exit()`, `hyprland/dispatch.c`) — unlike
  `hyprland_dispatch_focus_workspace()`'s own Lua-vs-classic dual-form probing,
  `exit` takes no arguments at all, so there's no argument-syntax ambiguity
  between the two config styles to work around; the plain classic form works on
  both. Restart/Shutdown run `systemctl reboot` / `systemctl poweroff` via a
  plain blocking `system()` call — acceptable here (unlike a bar module doing
  this on a recurring poll, which this project avoids elsewhere) because it's a
  one-shot, user-triggered action that's about to end the session anyway; a
  few hundred milliseconds of the event loop blocking on it is irrelevant.
- **Hyprland-keybind trigger, via `SIGUSR1` and a `signalfd`**: since stickBar
  has no IPC server of its own for an external command to message into (unlike
  Quickshell, which has `qs ipc call`), `main.c` blocks `SIGUSR1` via
  `sigprocmask()` and reads it back through a `signalfd` registered on the same
  epoll loop as everything else — the same "just another readable fd, no
  async-signal-safety concerns" shape every other event source in this project
  already uses, and notably *not* the classic `signal()` + flip-a-flag pattern
  `g_should_exit` (SIGINT/SIGTERM) uses, since that pattern only works for
  "the main loop should notice eventually," not "call `power_menu_toggle()`
  right now." **SIGUSR1 is blocked as the very first statement of
  `main()`** (`block_powermenu_signal()`), before anything can start a
  thread; the signalfd itself is still created later. POSIX signal masks are
  per-thread and inherited at creation, and a process-directed signal
  (`pkill`) goes to *any* thread that doesn't block it -- so every thread
  must inherit the block (the correct multi-threaded-process pattern for
  `signalfd`: block everywhere, only the thread holding the `signalfd`
  itself ever actually reads a pending instance). NVML's init starts threads of its
  own, so this must stay ahead of `sysmon_client_init()`. The `hyprland.conf` line is in [`README.md`](../../README.md)'s "Power menu keybind".

## In the Rust port

- **Files:** `src/modules/powermenu/mod.rs` (`PowerMenu`), `src/modules/powermenu/arch_logo.rs`,
  and the header/pill in `src/render/popup_chrome.rs`.
- **The overlay is its own `PopupSurface` kind** (`PopupSurface::overlay`): full-screen,
  overlay layer, compositor-sized. Its configure repaints it whenever the size changes, and
  `size()` is `(0, 0)` until then, which is what `menu` reports, so `test_power_menu`'s
  `wait_sized` works unchanged.
- **Each paint starts from a fresh image**, so the C "darker on every hover" bug can't happen.
  The dim is still painted with `Operator::Source` to state the intent.
- **The guard lives in `App::pointer_button`** (`src/wayland/pointer.rs`): `PowerMenu::click`
  only reports which card was picked and records it for `menu`, and the action runs only when
  not in test mode. Don't remove it, not even temporarily.
- **Restart/Shutdown don't block:** `systemctl` is spawned, and a thread named `power-action`
  waits for it. That logs a failure (e.g. polkit refusing) and reaps the process. The thread
  starts long after the signal source, so it inherits the blocked SIGUSR1. Logout goes through
  `Dispatcher::exit()` (`dispatch hl.dsp.exit()`, falling back to `dispatch exit`).
- **SIGUSR1** comes from calloop's signal source, set up first in `main.rs` since M1. Its
  handler calls `App::toggle_power_menu()`.
