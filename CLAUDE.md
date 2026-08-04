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

This is being built in **stages, not all at once**. The current state is a **minimal
skeleton**: bar shell + workspace indicator + clock only. Volume, mic, network, tray,
media player, system monitor, and the power-menu overlay are all still unimplemented —
see "Scope / roadmap" below before assuming a module exists.

## Running / testing changes

```sh
meson setup build      # first time only
meson compile -C build
./build/customBar
```

No test suite — this is a UI skeleton with no headless-testable logic beyond IPC
parsing. Validate changes by running the bar and observing it (it's fine to run
alongside the existing Quickshell bar; see below), plus watching stderr for Wayland
protocol errors or Hyprland IPC failures.

- **Running alongside Quickshell is safe.** customBar opens its own independent
  Wayland connection and surfaces; it shares no socket, lock file, or singleton
  resource with Quickshell. Both bars are `LAYER_TOP` with `exclusive_zone=20`
  anchored top/left/right, so wlroots (Hyprland) accumulates the zones additively —
  running both stacks them as two 20px strips (confirmed via
  `hyprctl monitors -j`'s `reserved` field going from `[0,20,0,0]` to `[0,40,0,0]`
  while customBar runs, and back to `[0,20,0,0]` after it exits cleanly).
- **Screenshotting for visual verification**: `grim` and `slurp` are installed.
  `grim -g "X,Y WxH" out.png` grabs a pixel region across the whole virtual screen
  (multi-monitor X offsets are cumulative, e.g. a 1920-wide first monitor puts the
  second monitor's origin at x=1920) — this is how rendering was checked during
  development, since `grim -o <output>` isn't compatible with `-g`.
- `hyprctl monitors -j` / `hyprctl -j workspaces` / `hyprctl -j activeworkspace` are
  useful for cross-referencing what the bar *should* be showing against what it
  actually renders.

## Architecture

### File layout

One subdirectory per concern, and one directory per bar widget under `modules/` —
this is a deliberate, explicit choice (see "Conventions" below), not the minimum
viable structure for a project this size.

```
customBar/
├── meson.build
├── protocols/
│   └── wlr-layer-shell-unstable-v1.xml   # vendored, see "Protocol codegen" below
└── src/
    ├── main.c                    entry point: wires everything up, owns the epoll
    │                             loop, owns teardown
    ├── config/
    │   ├── config.h / config.c   full Catppuccin Mocha palette + bar geometry/font
    │   │                         constants, ported from the Quickshell config.js
    │   └── glyphs.h / glyphs.c   Nerd Font icon codepoints + kanji numeral table,
    │                             plus glyph_utf8() to encode them at runtime
    ├── wayland/
    │   ├── context.h             AppContext + Output structs shared across every
    │   │                         wayland/* and bar/* file (see "Core data model")
    │   ├── registry.c            wl_registry global binding + the connect/roundtrip
    │   │                         sequence
    │   ├── event_loop.c          epoll setup + the wl_display prepare_read dance
    │   ├── output.c              wl_output tracking, geometry/mode/scale/done/name/
    │   │                         description handling, per-output index assignment
    │   ├── pointer.c             wl_seat -> wl_pointer, enter/motion/button routing
    │   └── shm_buffer.c          memfd-backed double-buffered wl_shm pool
    ├── bar/
    │   └── bar_surface.c         per-output zwlr_layer_surface_v1 lifecycle +
    │                             frame-callback-throttled redraw
    ├── render/
    │   ├── render.c              top-level per-output draw dispatch + background
    │   └── module_box.c          shared cairo/pango helper: bordered box chrome +
    │                             centered text, used by every widget
    ├── hyprland/
    │   ├── ipc_request.c         request socket (.socket.sock): one connection per
    │                             command, cJSON parsing helper
    │   ├── ipc_events.c          event socket (.socket2.sock): persistent
    │   │                         connection, line framing, event->refresh trigger
    │   └── dispatch.c            click-to-focus dispatch with the classic->Lua
    │                             fallback quirk (see "Hyprland IPC" below)
    └── modules/
        ├── workspaces/
        │   └── workspaces.c      occupied/focused set computation, kanji pills,
        │                         hit-rect recording + click handling
        └── clock/
            └── clock.c           timerfd-driven formatted time string + rendering
```

### Core data model

- **`AppContext`** (`wayland/context.h`) is the one struct almost everything holds a
  pointer to: the Wayland display/registry, bound globals (compositor, shm, seat,
  layer_shell), the pointer, and the `Output` linked list. `main.c` owns the single
  instance on its stack.
- **`Output`** (also `wayland/context.h`) is one tracked monitor. It carries a
  `wl_output*`, a `wl_surface*` (set by `bar_surface.c` once created — this is how
  `wayland/pointer.c` matches a `wl_pointer.enter`'s surface back to an `Output` by
  plain pointer comparison, no lookup table needed), an `index` (0-based, assigned by
  **binding order**, not by Hyprland's own monitor id/name — this is what
  `modules/workspaces` uses to assign each output its 5-wide workspace-id range: index
  0 -> ids 1-5, index N -> ids `[5N+1, 5N+5]`), and a `bar_surface` pointer that's
  `NULL` until the output's Wayland `done` event fires.
- **`BarSurface`** (opaque outside `bar/bar_surface.c`) is the real per-output
  render target: its `wl_surface`, `zwlr_layer_surface_v1`, double-buffered shm
  pool, `configured`/`dirty` flags, and pending frame callback. Everything else
  interacts with it only through `bar_surface_create()` / `_destroy()` /
  `_mark_dirty()`.
- Dependency direction is intentionally one-way: `wayland/*` doesn't depend on
  `bar/*`, `render/*`, or `modules/*` (with one exception — see below);
  `bar/bar_surface.c` depends on `render/render.h`; `render/render.c` depends on
  `modules/*`. `wayland/pointer.c` is the one place this is bent: it calls into
  `modules/workspaces/workspaces.h` directly for click hit-testing, since routing
  clicks through another layer of indirection wasn't worth it for a single module.
  If a second clickable module is added, revisit whether pointer.c should call a
  generic per-Output "handle_click" dispatch table instead of a specific module by name.

### Wayland plumbing gotchas (hit and fixed during initial implementation)

These aren't hypothetical — both actually broke the first build/run and are worth
knowing before touching this code:

- **Every listener struct field must be non-NULL, including ones for events you
  don't care about.** Binding `wl_output` at version 4 (for `scale`) but only
  filling in `geometry`/`mode`/`done`/`scale` in `wl_output_listener` crashes at
  runtime the moment the compositor sends a `name` or `description` event (added in
  v4) with `listener function for opcode 4 of wl_output is NULL`. `wayland/output.c`
  now has no-op `handle_name`/`handle_description` stubs for exactly this reason —
  if you bump a bind version anywhere, check the interface's full listener struct
  (`awk '/struct X_listener \{/,/^\};/' <header> | grep -E '\(\*\w+\)'`) and fill in
  every field, even as a no-op.
- **`xdg-shell.xml` is linked in even though nothing here uses `xdg_popup`.**
  `zwlr_layer_surface_v1.get_popup(popup: xdg_popup)` makes wayland-scanner's
  generated `wlr-layer-shell-*-protocol.c` reference the `xdg_popup_interface`
  *symbol* (not just the type) as part of that request's `wl_message` signature —
  compiling against a bare forward-declared type works fine, but linking fails with
  `undefined reference to 'xdg_popup_interface'` without xdg-shell's own generated
  code to define it. `meson.build` pulls `xdg-shell.xml` from the system
  `wayland-protocols` package (via `dependency('wayland-protocols')`'s
  `pkgdatadir`) purely to satisfy this, not because anything calls into it — don't
  be surprised its generated header is otherwise unused.
- **`memfd_create()` needs `#define _GNU_SOURCE` before any include, in the file
  that uses it** (`wayland/shm_buffer.c`). `c_std=gnu11` in `meson.build` does *not*
  imply this — that flag only changes which C *language* extensions the compiler
  accepts, not which glibc feature-test macros are predefined. Confirmed by direct
  test: `gcc -std=gnu11` alone gives `implicit declaration of function
  'memfd_create'`.
- **The `wl_display_prepare_read`/`read_events`/`cancel_read` dance
  (`wayland/event_loop.c`) is required, not optional**, because this bar mixes the
  Wayland display fd into an external `epoll_wait()` alongside a timerfd and the
  Hyprland event socket fd. `wl_display_dispatch()` alone always performs a
  blocking read if nothing's queued, which isn't safe to call just because
  `epoll_wait()` said *some* fd was ready.

## Hyprland IPC

Two independent connections, both handled without any third-party Wayland-adjacent
IPC library — just raw AF_UNIX sockets + cJSON:

- **Request socket** (`$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock`,
  `hyprland/ipc_request.c`): one connection per command (Hyprland's IPC doesn't
  pipeline). Write the command, `shutdown(fd, SHUT_WR)` (Hyprland only starts
  replying once it sees EOF on the write side — skipping this deadlocks), read to
  EOF, close. `j/<command>` gets JSON back (parsed with cJSON via
  `hyprland_ipc_request_json()`); plain commands like `dispatch ...` get plain text.
- **Event socket** (`.socket2.sock`, `hyprland/ipc_events.c`): one persistent
  non-blocking connection, registered on `main.c`'s epoll instance. Delivers
  `event>>payload\n` lines, which may arrive split across reads — `ipc_events.c`
  buffers and re-frames them. On any event that can change workspace
  occupancy/focus (`workspace`, `workspacev2`, `focusedmon`, `focusedmonv2`,
  `openwindow`, `closewindow`, `movewindow`, `movewindowv2`, `create`/
  `destroyworkspace[v2]`), it calls `workspaces_refresh()` (a fresh
  `j/workspaces` + `j/activeworkspace` query — no incremental delta tracking,
  deliberately, since re-querying is cheap at this event rate) and marks every
  output dirty. If the connection drops (EOF/error), `ipc_events.c` returns `false`
  and **does not retry itself** — `main.c` owns the "attempt exactly one reconnect,
  then give up on live workspace updates for this run" policy, so that policy
  lives in one place instead of being split across files.
- **`hyprctl -j workspaces` objects already carry a `windows` count** — occupancy is
  just `windows > 0`; there's no need to separately query `clients`/toplevels the
  way the Quickshell/QML version does via `Hyprland.toplevels`.
- **Dispatch syntax is compositor-config-dependent, and this matters.**
  `hyprland/dispatch.c`'s `hyprland_dispatch_focus_workspace()` tries
  `dispatch hl.dsp.focus({ workspace = <id> })` **first** — **no surrounding
  quotes**. `hyprctl dispatch '...'` on the CLI only works because the *shell*
  strips those quotes before hyprctl sees them; this code writes straight to the
  raw socket with no shell involved, so a first version that included literal
  `'...'` quote characters in the command text got silently rejected by
  Hyprland's Lua parser (`expected a dispatcher`) — confirmed by testing both the
  quoted and unquoted forms directly against the socket with a throwaway Python
  script, bypassing `hyprctl` entirely. This is confirmed to work on this
  machine's Lua-configured Hyprland (0.56.1), and matches what the reference
  Quickshell bar's own click handler sends **unconditionally, with no fallback at
  all**.
  - **`hl.dsp.exec_raw("workspace <id>")` looks like it should be the "right"
    config-agnostic escape hatch** (it exists specifically to run a classic
    dispatcher string under a Lua config, per the Hyprland wiki and a related
    Waybar issue) — but on this exact build it returns `ok` without actually
    switching the workspace, confirmed live against the socket. **Don't reach for
    it as a fix without re-testing it directly against the socket first** — it
    looked correct on paper and wasn't.
  - Only if the Lua form is rejected does the code fall back once to the classic
    `dispatch workspace <id>` syntax (for compatibility with non-Lua configs), and
    logs to stderr if *that* also fails, instead of failing silently.
  - **Which form actually worked is cached in a static `DispatchStyle` after the
    first call** (a given Hyprland instance's dispatch style doesn't change at
    runtime). On this machine's Lua config, the very first attempt already
    succeeds, so **no stderr output should ever appear in normal use** — earlier
    iterations tried the classic form first (which always fails here, needing a
    fallback + one-time log) and, before the caching was added, re-probed and
    re-logged the rejection on *every single click*. If you see rejection
    messages in the log again — especially repeated ones — one of these
    regressed.

## Conventions

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

## Known limitations (first pass, not bugs)

- **No HiDPI/fractional-scale handling.** `wl_output.scale` is received and
  discarded (`wayland/output.c`'s `handle_scale`) — fine on this machine (both
  monitors report `scale: 1.00`), but buffers are always allocated at 1x.
- **No live output reconfiguration.** A second `wl_output.done` burst after the
  first (e.g. a real resolution change) is detected and logged, but the bar
  surface isn't resized in response.
- **Output unplug/replug can shift workspace ranges**, since `Output.index` is
  assigned by binding order at connect time, not tied to a stable monitor
  identity.
- **No pointer cursor image on hover** — clicks work, but the cursor glyph doesn't
  change when hovering the bar. `wayland-cursor` isn't even linked yet; add it
  back to `meson.build`'s dependencies when this is implemented.
- **No popups/tooltips of any kind yet** — this is why `xdg-shell.xml` is linked
  only for its `xdg_popup_interface` symbol and not otherwise used; a real hover
  popup (e.g. for a future network/volume module) will need actual `xdg_popup`
  usage, not just the symbol reference.

## Scope / roadmap

Implemented: bar shell (per-output layer-surface, double-buffered shm rendering),
workspace indicator (kanji pills, occupied/focused logic, click-to-focus), clock.

Not yet implemented (planned for later passes, in no particular order): volume, mic,
network, system tray, media player (MPRIS), CPU/GPU/memory monitor, power-menu
overlay. When adding one, follow the existing module shape: a `modules/<name>/`
directory, a render function taking `(cairo_t*, ..., x, box_top, box_height)` and
returning the next x coordinate, and — if it needs live data — either a Hyprland IPC
event hook (if relevant) or its own poll/event source integrated into `main.c`'s
epoll loop the same way the clock's timerfd and Hyprland's event socket are.
