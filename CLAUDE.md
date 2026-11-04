# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A from-scratch C implementation of a Wayland status bar for Hyprland, built directly
against `wl_compositor`/`wl_shm`/`zwlr_layer_shell_v1` and drawn with cairo/pangocairo
— no GTK, no other toolkit. It's modeled visually and behaviorally on the user's
existing Quickshell/QML bar at `~/Dotfiles/quickshell/.config/quickshell/` (colors,
fonts, module conventions, and per-widget behavior there are the source of truth this
project is echoing in C — consult that repo's own `CLAUDE.md` and source when adding a
module, rather than guessing at parity).

## Build and test

```sh
meson setup build      # first time only
meson compile -C build
./build/stickBar
```

```sh
meson compile -C build && python3 tests/run_all.py   # ~2 min, extra bars flash briefly
python3 tests/perf_compare.py --against HEAD~1        # CPU/memory/compositor table vs. a ref
```

Run the bar and watch stderr for Wayland protocol errors or Hyprland IPC failures.

**Test safety**, details in [`docs/testing.md`](docs/testing.md):

- Use the test-control channel, not ydotool, for anything popup-related: ydotool moves the
  user's real cursor.
- Never remove the power menu's test guard. It's what stops a test logging out or
  rebooting ([`docs/modules/power-menu.md`](docs/modules/power-menu.md)).
- Never point a test at the real DAC script
  ([`docs/modules/eq.md`](docs/modules/eq.md)).

## Documentation library

Read the file for what you're touching before changing it.

| Read | When | Covers |
|---|---|---|
| [`docs/architecture.md`](docs/architecture.md) | Finding your way around, or changing how redraws work | File layout, `AppContext`/`Output`, redraw path, measured performance; `main.c`, `bar/bar_surface.c`, `render/` |
| [`docs/testing.md`](docs/testing.md) | Testing or verifying anything | Test-control channel, cross-reference commands, screenshots, multi-monitor gotcha; `testing/`, `tests/` |
| [`tests/README.md`](tests/README.md) | Adding a test, or finding what guards something | What each test checks and what bug it catches |
| [`docs/popups.md`](docs/popups.md) | Adding or changing any dropdown, or what a mouse button does | Button table, popup look, margins, anchoring, catchers, resizing; `bar/popup_surface.c` |
| [`docs/wayland.md`](docs/wayland.md) | Touching Wayland plumbing or build setup | Listener, linking, `memfd`, event-loop and buffer rules; `wayland/` |
| [`docs/limitations.md`](docs/limitations.md) | Wondering if something is intentionally missing | Known limitations and fixed-array caps |
| [`BUG.md`](BUG.md) | Fixing a bug, or after finding one | Open bugs, and how fixed ones happened |
| [`README.md`](README.md) | User-facing setup and features | Install, dependencies, the power menu keybind |

Modules:

| Read | When touching | Covers |
|---|---|---|
| [`docs/modules/hyprland.md`](docs/modules/hyprland.md) | Workspaces, Hyprland requests or events | `hyprland/`, `modules/workspaces/` |
| [`docs/modules/tray.md`](docs/modules/tray.md) | The system tray, SNI, DBusMenu, sd-bus | `dbus/`, `modules/tray/` |
| [`docs/modules/audio.md`](docs/modules/audio.md) | Volume, mic, device dropdown, mixer, scrolling | `audio/`, `modules/volume/`, `modules/mic/` |
| [`docs/modules/sysmon.md`](docs/modules/sysmon.md) | CPU, GPU, memory, NVML | `sysmon/`, `modules/cpu/`, `modules/gpu/`, `modules/mem/` |
| [`docs/modules/clock.md`](docs/modules/clock.md) | The year calendar | `modules/clock/` |
| [`docs/modules/media.md`](docs/modules/media.md) | MPRIS and the media widget | `mpris/`, `modules/media/` |
| [`docs/modules/network.md`](docs/modules/network.md) | Network status, rates, its graph | `network/`, `modules/network/`, `render/braille_graph.c` |
| [`docs/modules/privacy.md`](docs/modules/privacy.md) | Mic/webcam/screen-share indicator, PipeWire | `pipewire/`, `modules/privacy/` |
| [`docs/modules/power-menu.md`](docs/modules/power-menu.md) | Power menu, Arch logo, SIGUSR1 | `modules/powermenu/` |
| [`docs/modules/eq.md`](docs/modules/eq.md) | DAC EQ, state file, profile switching | `eq/`, `modules/eq/` |

## Conventions

- **Bugs go in `BUG.md`**: open ones, and the history of fixed ones. In the docs, keep
  only the rule a bug taught.
- **Docs live in `docs/`, one topic per file, each fact in one place.** Keep this file an
  index: new detail goes in the topic file, and a new file gets a row in the table below.
- **This project is commented far more heavily than this assistant's usual
  default.** That's deliberate, per explicit user instruction: comment liberally
  wherever logic isn't self-explanatory from names alone (protocol quirks,
  non-obvious sequencing, buffer-lifecycle rules) — keep doing that for new code
  here, even though it's not how other repos get treated.
- **One file/directory per concern, one directory per bar widget under
  `modules/`**, even when (as now) there are only one or two widgets — also
  explicit user preference, meant to keep the codebase easy to navigate as more
  modules land. Don't collapse this back into fewer, larger files for convenience.
- Styling constants flow through `config/config.h`'s `COLORS` / `BAR` / `FONT` —
  add new shared tokens there, but keep one-off widget-specific tuning (e.g. the
  workspace pill's own 4px border width and 14px/6px padding, which intentionally
  don't match `BAR.module_border_width`) local to that widget's `.c` file, matching
  the convention the reference Quickshell project itself documents and uses.
- Nerd Font / CJK glyph codepoints are never hand-typed as raw UTF-8 literals in
  source — they're numeric `#define`s in `config/glyphs.h`, verified against the
  installed font via fontTools (or `ord()` for the kanji) before being written, and
  encoded to UTF-8 at runtime by `glyph_utf8()`. Follow this pattern for any new
  icon; see the reference project's own `CLAUDE.md` for why (truncated multi-byte
  sequences are an easy, silent mistake otherwise).
- Rendering text always goes through `render/module_box.c`'s pangocairo helpers
  (`module_box_measure_text` / `module_box_draw_text_centered`), not cairo's toy
  text API — the toy API doesn't reliably shape/substitute PUA icon glyphs or CJK
  characters through the Nerd Font.

## Scope / roadmap

Nothing from the original reference-bar feature set remains unported; the EQ and
privacy modules have no reference equivalent. Future additions should follow the
existing module shape: a `modules/<name>/` directory, a render function taking
`(cairo_t*, ..., x, box_top, box_height)` and returning the next x coordinate, and
— if it needs live data — either a Hyprland IPC event hook (if relevant) or its
own poll/event source integrated into `main.c`'s epoll loop like every other fd
there.
