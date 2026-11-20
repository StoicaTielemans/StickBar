# Power menu and the Arch logo

Files: `src/modules/powermenu/mod.rs` (`PowerMenu`: the overlay, its `CARDS` table),
`src/modules/powermenu/arch_logo.rs` (the bar box), the header and pill in
`src/render/popup_chrome.rs`, and the actions in `src/wayland/pointer.rs`.

As in the reference (`ArchLogoWidget.qml` toggling a `PowerMenuState`), the bar's leftmost box
is the trigger and the overlay is separate; every trigger toggles. **A right click on the Arch
logo opens it** (the user's choice, like every other box's menu); a left click does nothing.

## The Arch logo

- **A cutout, not the usual bordered box**: a solid `COLORS.sapphire` fill with the Arch glyph
  in `COLORS.mantle` (the bar's background), no border, so the logo reads as a shape cut out
  of a block.
- **Sized from the glyph's *ink*** (`TextCache::ink_width`) plus 12 px, not its advance width:
  this glyph draws 14 px of ink on an 8 px advance, and sizing from the advance cut it off
  ([BUG.md](../../BUG.md) "Power menu").

## The overlay

- **One full-screen surface per output** (`PopupSurface::overlay`: overlay layer, all four
  edges anchored, compositor-sized), like the reference's `Variants { model:
  Quickshell.screens }`. It's its own click-catcher: a click on a card runs that action, a
  click elsewhere on the panel is absorbed, a click on the dimmed background closes the menu.
  The menu closes before an action runs. Its configure repaints it whenever the size changes;
  `size()` is `(0, 0)` until then (`test_power_menu`'s `wait_sized` relies on it).
- **The panel is centred on the surface's real size**, not the output's height: the surface
  stops at the bars' exclusive zones.
- **The dim is 30 % `COLORS.crust`** (the reference's `"#4D" + crust`), painted with
  `Operator::Source`. Each paint starts from a fresh image, so translucent layers can't pile up
  on hover (the C bar got darker on every hover).
- **The panel**: a fixed 388×168 `mantle` panel with a `sapphire` border (the Arch box's
  colour); a header with the power glyph, `Power · user@host` and an uptime pill (both read at
  open; no pill if `/proc/uptime` can't be read); three `base` cards in a row, each with its
  icon (`FONT.display`, 32 px), name and a one-line description. On hover a card lightens and
  its border and name take the action's colour (Logout yellow, Restart sky, Shutdown red).
  It's an overlay rather than a dropdown because the keybind has no box to anchor to.

## Actions

- **Logout**: `Dispatcher::exit()` over Hyprland's request socket (`dispatch hl.dsp.exit()`,
  falling back to `dispatch exit`; [hyprland.md](hyprland.md)).
- **Restart / Shutdown**: `systemctl reboot` / `systemctl poweroff`, started through
  `util::spawn` like every child of the bar ([architecture.md](../architecture.md), "Child
  processes"). A `power-action` thread waits for it, logs a failure (e.g. polkit refusing)
  and reaps it. No confirmation step ([limitations.md](../limitations.md)).
- All three have been checked by hand on the real session. No test runs them, by design:

**The test guard.** In test mode, `PowerMenu::click` only reports which card was picked and
records it for `menu`'s `power_last_action`; `App::pointer_button` runs the action only when
not in test mode. `tests/test_power_menu.py` adds a second layer: its bar has a fake
`systemctl` first on its `PATH` (which is why `systemctl` is found through PATH, not by
absolute path) and a Hyprland signature matching no socket, and the test fails if the fake
ever ran. **Never remove the guard to test something**, not even temporarily, on a session
someone is using.

## The keybind: SIGUSR1

The bar has no IPC server (unlike Quickshell's `qs ipc call`), so the Hyprland keybind sends
`SIGUSR1` (`pkill -SIGUSR1 -x stickBar`; the `hyprland.conf` line is in
[`README.md`](../../README.md), "Power menu keybind"). calloop's signal source turns it into an
event that calls `App::toggle_power_menu()`.

**Rule: the signal source is created first in `main()`, before anything can start a thread.**
A signal source works by blocking the signals (SIGINT, SIGTERM, SIGUSR1) and reading them
from a file descriptor. Signal masks are per-thread and inherited at creation, and a
process-directed signal goes to *any* thread that doesn't block it, where SIGUSR1's default
action kills the process. Every thread (the bar's own, NVML's, zbus's) must start after the
block. `test_power_menu` checks every thread's `SigBlk` and sends real SIGUSR1s to a test
instance. A child process must not inherit the block ([architecture.md](../architecture.md),
"Child processes").
