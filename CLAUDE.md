# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

**The Rust bar is the bar.** It was ported from C one module at a time (the record,
milestones M0–M15 and every measurement, is [`docs/porting.md`](docs/porting.md)). The C
bar is kept on the `c-legacy` branch; until switch day's merge it's still on `main`,
checked out in the sibling folder `../StickBar`, and the Rust bar is on `rust-rewrite`
(this folder) through a daily-use trial.

`docs/` and `BUG.md` were written for the C implementation and stay as the spec and the
history of hard-won lessons; each module doc ends with an **"In the Rust port"** section
describing the Rust code. Where the two disagree, the Rust code and those sections are
the truth.

A from-scratch Wayland status bar for Hyprland (originally in C), built directly
against `wl_compositor`/`wl_shm`/`zwlr_layer_shell_v1` and drawn with cairo/pangocairo
— no GTK, no other toolkit. It's modeled visually and behaviorally on the user's
existing Quickshell/QML bar at `~/Dotfiles/quickshell/.config/quickshell/` (colors,
fonts, module conventions, and per-widget behavior there are the source of truth this
project is echoing — consult that repo's own `CLAUDE.md` and source when adding a
module, rather than guessing at parity).

## Build and test

Two builds, and **every build compiles both** (user's standing rule):
`target/debug/stickBar` is the debug build (what the tests run against),
`target/release/stickBar` is the production build (what the user runs day to day).
`./build.sh` does everything in order and stops at the first failure: `cargo fmt
--check`, `cargo clippy --all-targets` (the lint policy below), the debug build,
then the release build.

```sh
./build.sh
./target/release/stickBar
```

```sh
./build.sh && python3 tests/run_all.py                # ~2 min, extra bars flash briefly
python3 tests/perf_compare.py --bin target/release/stickBar \
    --against <C bar's build-release/stickBar>         # vs. the C bar (c-legacy), release builds
python3 tests/perf_compare.py --against HEAD~1         # vs. any earlier ref (C or Rust)
```

Two tests depend on this machine, not the code: `test_sysmon_data` fails while other
apps hold more memory than its 400 MB child, and `test_nvml_vs_smi` mis-parses driver
620's fractional temperature. Both fail the same way against the C bar.

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
| [`docs/porting.md`](docs/porting.md) | Choosing a crate, or wondering why the Rust bar does something differently from C | The port's milestones, crate choices with reasons, every measurement |
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
| [`docs/modules/media.md`](docs/modules/media.md) | MPRIS, the media widget, its spectrum visualizer | `mpris/`, `modules/media/`, `visualizer/` |
| [`docs/modules/network.md`](docs/modules/network.md) | Network status, rates, its graph | `network/`, `modules/network/`, `render/braille_graph.c` |
| [`docs/modules/privacy.md`](docs/modules/privacy.md) | Mic/webcam/screen-share indicator, PipeWire | `pipewire/`, `modules/privacy/` |
| [`docs/modules/power-menu.md`](docs/modules/power-menu.md) | Power menu, Arch logo, SIGUSR1 | `modules/powermenu/` |
| [`docs/modules/eq.md`](docs/modules/eq.md) | DAC EQ, state file, profile switching (shown inside the media module) | `eq/`, `modules/media/eq_section.rs` |

## Rust rules

The lint policy lives in `Cargo.toml`'s `[lints]` table, with the reasoning for
each lint next to it. It makes these build errors:

- any `unsafe` (`forbid`, so no `#[allow]` can override it);
- every warning;
- the crash shortcuts: `unwrap`, `expect`, `panic!`, `todo!`, `unimplemented!`,
  `unreachable!`, slice indexing `v[i]`, string slicing `&s[a..b]`, and `process::exit`;
- the silent-bug shortcuts: `as` casts, `mem::forget`, and ignored `Result`s or
  other `#[must_use]` values;
- `println!`/`dbg!`: diagnostics go to stderr with `eprintln!`;
- any `#[allow(...)]` at all;
- a missing `///` doc comment on any item (fn, struct, field, enum variant);
- a module written as `foo.rs` + `foo/`: every module is a directory with `mod.rs`;
- raw non-ASCII in literals (glyphs are `'\u{...}'` consts);
- blocking calls on the event loop (`thread::sleep`, `Command::output`/`status`) and
  unnamed threads (`thread::spawn`). `clippy.toml`'s `disallowed-methods` gives the
  reason in the error.

Formatting is `rustfmt.toml`. `clippy.toml` relaxes only the crash shortcuts, and only
inside `#[cfg(test)]`. A misspelled lint name is an error too, so the config can't
silently drift.

- **Never weaken a lint, add a suppression, or edit `Cargo.toml`'s `[lints]` /
  `clippy.toml` to make code compile. Fix the code instead.** Handle the case with `match`, `?`,
  `if let`, `.get()`, `try_from` or a default. If a lint truly can't be satisfied,
  stop and ask the user. If one is agreed, it's `#[expect(lint, reason = "...")]`,
  which turns into an error itself once it's no longer needed.
- No `unsafe`, not even for FFI. Use the safe wrapper crates (smithay-client-toolkit,
  cairo-rs/pango, zbus, pipewire, libpulse-binding, nvml-wrapper, rustix). If
  something truly has no safe wrapper, discuss it with the user before doing
  anything; the answer would be a separate tiny crate, never `unsafe` in this one.
- Don't paper over borrow-checker errors with `.clone()` or `Rc<RefCell<…>>` by
  reflex. Restructure ownership first, and if shared mutable state is genuinely
  needed, say why in a comment.
- Every module sets `clickable: true` on the `ModuleBox` of any box that reacts to a click
  (any button), and every dropdown reports its click targets to `App::cursor_shape_at`, so
  the pointer shows a hand there (a Rust-only addition: `docs/porting.md`).
- Worker threads (pulse, PipeWire, D-Bus) use `thread::Builder::new().name(..)`, start
  only after the SIGUSR1 signal source exists, and talk to the main loop through a
  calloop channel.
- Add a crate only when the module that needs it is being ported, and keep
  `Cargo.lock` committed.

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
- Styling constants flow through `src/config/`'s `COLORS` / `BAR` / `FONT` (the C
  bar's `config/config.h`) —
  add new shared tokens there, but keep one-off widget-specific tuning (e.g. the
  workspace pill's own 4px border width and 14px/6px padding, which intentionally
  don't match `BAR.module_border_width`) local to that widget's own file, matching
  the convention the reference Quickshell project itself documents and uses.
- Nerd Font / CJK glyphs are named `char` consts in `src/config/glyphs.rs`, written
  as `'\u{f0e7}'` escapes, never pasted as raw glyphs. Verify each codepoint against
  the installed font with fontTools (or `ord()` for the kanji) before writing it.
  Rust strings are always valid UTF-8, so the C bar's runtime `glyph_utf8()` encoder
  isn't needed.
- Text is always rendered through the pango helpers in `src/render/` (the C bar's
  `render/module_box.c`), never cairo's toy text API: the toy API doesn't reliably
  shape PUA icon glyphs or CJK characters through the Nerd Font.

## Scope / roadmap

Crate choices and the port's history: [`docs/porting.md`](docs/porting.md). Every module has
the C bar's shape: a `src/modules/<name>/` directory and a render function taking the
cairo context, x and the box's top and height, returning where its box landed. If it needs
live data, it joins the single calloop event loop in `main.rs`: an fd source, a timer, or a
channel from its worker thread.
