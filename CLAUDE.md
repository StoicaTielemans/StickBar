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

This is being built in **stages, not all at once**. The current state has: bar shell,
workspace indicator, clock, and system tray. Volume, mic, network, media player, system
monitor, and the power-menu overlay are all still unimplemented — see "Scope / roadmap"
below before assuming a module exists.

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
    │   ├── bar_surface.c         per-output zwlr_layer_surface_v1 lifecycle +
    │   │                         frame-callback-throttled redraw (lives forever,
    │   │                         one per output)
    │   └── popup_surface.c       generic *transient* layer-surface: explicit
    │                             anchor/margin/size, single-shot render (no
    │                             frame-callback throttling), created per-open and
    │                             destroyed on close -- used by the tray's context
    │                             menu and its click-catcher surfaces, written
    │                             generically so a future tooltip/popup can reuse it
    ├── render/
    │   ├── render.c              top-level per-output draw dispatch + background
    │   └── module_box.c          shared cairo/pango helpers: bordered box chrome,
    │                             centered text, left-aligned text (added for the
    │                             tray's menu rows) -- used by every widget
    ├── hyprland/
    │   ├── ipc_request.c         request socket (.socket.sock): one connection per
    │                             command, cJSON parsing helper
    │   ├── ipc_events.c          event socket (.socket2.sock): persistent
    │   │                         connection, line framing, event->refresh trigger
    │   └── dispatch.c            click-to-focus dispatch with the classic->Lua
    │                             fallback quirk (see "Hyprland IPC" below)
    ├── dbus/                     D-Bus plumbing for the system tray (see "D-Bus /
    │                             system tray" below for the full protocol writeup)
    │   ├── dbus_loop.c           sd_bus connection + generic epoll integration
    │   ├── sni_watcher_service.c attempts to *become* the StatusNotifierWatcher
    │   │                         (claims the well-known bus name, hosts the
    │   │                         /StatusNotifierWatcher object); tried first
    │   ├── sni_watcher.c         StatusNotifierWatcher Host (client) registration;
    │   │                         fallback used only if _service.c's claim fails
    │   ├── sni_registry.c        tracked-item table (fixed-array, like
    │   │                         workspaces.c's convention), property fetch,
    │   │                         per-item signal subscriptions
    │   ├── sni_icon.c            pure IconPixmap -> cairo_surface_t* conversion
    │   │                         (no D-Bus types in this file, deliberately)
    │   ├── icon_theme.c          fallback PNG icon lookup for items with no
    │   │                         usable IconPixmap (IconThemePath override,
    │   │                         then a pragmatic hicolor/pixmaps search) --
    │   │                         also no D-Bus types, pure filesystem + cairo
    │   └── dbusmenu.c            com.canonical.dbusmenu client: GetLayout parsing,
    │                             Event("clicked",...) triggering
    └── modules/
        ├── workspaces/
        │   └── workspaces.c      occupied/focused set computation, kanji pills,
        │                         hit-rect recording + click handling
        ├── clock/
        │   └── clock.c           timerfd-driven formatted time string + rendering
        └── tray/
            ├── tray.c            icon row render + hit-rect recording + click
            │                     routing (Activate vs open-menu)
            └── tray_context_menu.c   popup layer-surface + click-catchers +
                                  hover/click handling for the DBusMenu entries
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
  `modules/*`. `wayland/pointer.c` is the one place this is bent: it calls
  directly into `modules/workspaces/workspaces.h` *and* `modules/tray/tray.h` /
  `modules/tray/tray_context_menu.h` for click routing — two explicit call sites,
  not a generic dispatch table (see "D-Bus / system tray" below for why that
  still isn't worth building at N=2 clickable bar-surface modules). Revisit if a
  third one lands.
- **`AppContext.pointer_over` vs `pointer_over_surface`**: `pointer_over_surface`
  is the raw `wl_surface*` currently under the pointer, always kept up to date.
  `pointer_over` is the `Output*` it resolves to *if* it's one of the bar's own
  surfaces — `NULL` for a tray popup/click-catcher surface, since those are never
  stored on any `Output` (only `bar_surface.c`-owned surfaces are). `pointer.c`'s
  button handler checks the tray context menu's surfaces first (via
  `tray_context_menu_is_menu_surface()` / `_is_catcher_surface()`), *then* falls
  back to the `Output`-keyed path — a popup/catcher surface never matches an
  `Output`, so this ordering is required, not just tidier.

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

## D-Bus / system tray

The tray talks two separate freedesktop-ecosystem protocols over the D-Bus
session bus via `sd-bus` (`libsystemd`, chosen over `dbus-1`/libdbus for a
leaner, more modern C API — confirmed present via `pkg-config --modversion
libsystemd`): StatusNotifierItem (SNI, the tray-icon protocol) and DBusMenu (the
right-click menu protocol). Both were reverse-verified against this machine's
*real* running tray items (`busctl --user ...` against a live EasyEffects and
Steam) rather than only against spec text — several details below only became
clear that way.

- **sd-bus is driven from the same external epoll loop as everything else**
  (`dbus/dbus_loop.c`), using the same shape as the Wayland/Hyprland fds already
  in `main.c`: `sd_bus_get_fd()` for the fd, `sd_bus_process()` looped until it
  returns 0 to drain available work, then `sd_bus_get_events()` /
  `sd_bus_get_timeout()` re-applied *every* pump (not just once at connect) since
  sd-bus's interest mask and internal deadline both change over time (e.g. wants
  `EPOLLOUT` mid-call). The timeout is a genuinely separate timerfd, converted
  from sd-bus's absolute `CLOCK_MONOTONIC` microsecond deadline to a relative
  `itimerspec` — this is what makes sd-bus's own call-timeout bookkeeping fire at
  all when it isn't running its own default event loop.
- **customBar can act as either the StatusNotifierWatcher itself or a Host
  client of someone else's, decided automatically at startup, with no other
  code needing to know or care which mode is active.** `main.c` always tries
  `dbus/sni_watcher_service.c`'s `sni_watcher_service_try_start()` first — it
  attempts `sd_bus_request_name(bus, "org.kde.StatusNotifierWatcher", 0)`, and
  if that succeeds, registers the `/StatusNotifierWatcher` object itself
  (`RegisterStatusNotifierItem`/`RegisterStatusNotifierHost`,
  `RegisteredStatusNotifierItems`/`IsStatusNotifierHostRegistered`/
  `ProtocolVersion`, and the `StatusNotifierItem{,Un}Registered`/
  `StatusNotifierHostRegistered` signals) so items register directly with this
  process. Only if that name is already owned by something else (confirmed
  live by running with Quickshell stopped entirely: customBar claimed
  `org.kde.StatusNotifierWatcher` itself, and both EasyEffects and Steam showed
  up in `RegisteredStatusNotifierItems` with no other Watcher-providing process
  running) does `main.c` fall back to the original, unchanged Host-only path in
  `dbus/sni_watcher.c`'s `sni_watcher_register_host()` — connecting as a client
  to whichever Watcher does exist instead. `dbus/sni_registry.c`,
  `modules/tray/tray.c`, etc. are identical in both modes; they only ever see
  `sni_registry_add()`/`_remove()` calls, regardless of which path triggered
  them. `sni_watcher_service_try_start()` also best-effort claims
  `org.freedesktop.StatusNotifierWatcher` as a second name (failure ignored)
  for any host hardcoded to look for that name instead, reusing the same
  vtable/object registration.
  - **Item registration accepts both real-world calling conventions**, the same
    two `sni_watcher.c`'s client-side `split_bus_name_path()` already parses:
    if the `RegisterStatusNotifierItem(service)` argument starts with `/`, it's
    an object path and the bus name is the caller's own unique name (via
    `sd_bus_message_get_sender()`) — what Steam does; otherwise `service` is
    itself the bus name and the path defaults to `/StatusNotifierItem` — the
    KDE convention EasyEffects uses.
  - **Auto-unregister on item process exit** subscribes to
    `org.freedesktop.DBus`'s `NameOwnerChanged(sss)` signal (this fires for
    *every* bus-name ownership change system-wide, not just ones customBar
    tracks — D-Bus match rules can't filter down to "only names I've
    registered items under," so every firing is checked against the registry).
    An empty `new_owner` means that name's previous owner disconnected;
    `sni_registry_remove_by_bus_name()` then removes every item registered
    under that bus name (a single client registering more than one item is
    rare but possible, so this isn't just `sni_registry_remove()` on one
    (bus_name, path) pair). Without this, closing an app would leave its icon
    stuck in the tray forever with no other signal ever telling customBar it's
    gone — this path has no precedent elsewhere in the codebase to lean on and
    was the one part of this feature confirmed only via a live
    start-app/quit-app/watch-icon-disappear test, not by reasoning about the
    spec text.
  - **Not implementing the Watcher role was the prior limitation this removes.**
    A machine with genuinely nothing providing *any* Watcher (neither
    customBar's own service nor a fallback like Quickshell/KDE/snixembed)
    would previously have left the tray silently empty forever; now customBar
    itself is that provider by default.
- **Bus-name/object-path splitting**: `RegisteredStatusNotifierItems` and the
  `StatusNotifierItem{,Un}Registered` signals give strings like
  `":1.30/StatusNotifierItem"` or `":1.52/org/ayatana/NotificationItem/steam"` —
  confirmed both forms occur on real items. Rule: the first `/` splits bus name
  from path; no `/` at all means the whole string is the bus name and the path
  defaults to `/StatusNotifierItem`.
- **IconPixmap byte format — verified pixel-by-pixel against real wire data, not
  assumed.** SNI pixmap bytes are ARGB32 in network byte order (big-endian); a
  per-pixel `ntohl()` on the raw 4 bytes recovers `0xAARRGGBB` in host-native
  form, which is exactly what cairo's `CAIRO_FORMAT_ARGB32` wants stored as a
  host-native `uint32_t` (`dbus/sni_icon.c`). This was confirmed correct by
  dumping raw wire bytes and the converted output side by side and checking the
  alpha channel traces out a coherent icon glyph shape.
  - **Discovered while verifying this, not a bug**: some real apps (EasyEffects
    included, via its `-symbolic`-suffixed icon name) ship IconPixmap as a
    flat-colored *mask* — every opaque pixel carries the *exact same* dim,
    low-contrast RGB, meant to be recolored at display time to the desktop
    theme's foreground color. The reference Quickshell bar renders this same
    item bright/legible because Qt/KDE's full icon-theme integration recolors it
    via `IconName`, not the raw pixmap. This project doesn't do real theme
    lookups, so instead `sni_icon_convert()` now applies a cheap heuristic
    directly on the pixmap bytes (`detect_symbolic_mask()` in
    `dbus/sni_icon.c`, run once per icon before the premultiply pass): sample
    the straight RGB of every pixel above alpha 40/255 (low-alpha edge pixels
    are noisy and shouldn't count), and if the max-min range across R/G/B among
    those samples is within 12/255 *and* the shared color's Rec. 601 luminance
    is under 40%, treat it as a mask and override every pixel's RGB to
    `COLORS.text` before premultiplying, leaving alpha (the glyph shape)
    untouched. Scoped to this IconPixmap path only, deliberately not applied to
    `dbus/icon_theme.c`'s PNG-file fallback path — a flat-colored mask is a
    specifically IconPixmap-shaped problem, and applying the same heuristic to
    a normal multi-color raster app logo loaded from an icon theme file risks
    wrongly recoloring a legitimately monochrome icon. Confirmed working live:
    with Quickshell down (see below) and customBar as its own Watcher,
    EasyEffects' tray icon renders in this bar's light `COLORS.text` instead of
    its dim baked-in color, while Steam's icon (no IconPixmap at all, resolved
    via the PNG fallback instead) is unaffected.
  - Premultiplication: cairo wants premultiplied alpha; SNI pixmaps are
    conventionally *not* premultiplied on the wire, so `sni_icon_convert()`
    premultiplies unconditionally — also confirmed correct via the same
    byte-level check.
- **DBusMenu's `Event` method is fire-and-forget (`NoReply`)** — confirmed
  against the real `com.canonical.dbusmenu` interface definition. Sent via
  `sd_bus_message_set_expect_reply(msg, 0)` + `sd_bus_send()`, not
  `sd_bus_call_method()` (which blocks waiting for a reply that a `NoReply`
  method may never send — and blocking at all here would freeze the whole bar's
  event loop until the target app responds, since this is a single-threaded
  process). `sni_item_activate()`'s `Activate()` call uses the same fire-and-forget
  `sd_bus_send()` pattern for the same reason, even though `Activate` isn't
  `NoReply` — any eventual reply just arrives with no pending-call slot
  registered to receive it and is silently dropped by sd-bus, which is normal,
  not an error.
- **`GetLayout`'s `recursionDepth=1`** naturally produces the flat,
  non-recursive entry list the reference `TrayContextMenu.qml` renders — no
  extra filtering needed. A live `GetLayout(0, 1, [])` against a real menu
  confirmed the shape: entries without a `"type"` property are normal clickable
  items, `"type":"separator"` marks a separator, and `"enabled"` **defaults to
  `true` if the property is absent** (matters for correctness, not just a spec
  footnote — most real entries omit it).
- **The has-menu-overrides-button click rule** (`modules/tray/tray.c`'s
  `tray_handle_click()`): re-reading the reference `Tray.qml`'s actual
  `onClicked` handler shows that if an item has a menu, **any** click — left or
  right — opens it; `Activate()` only ever fires when the item has **no** menu
  at all, and only then on left-click. This is easy to get backwards (assuming
  "left activates, right opens the menu" by convention) — menu presence
  overrides button entirely. Both live test items here (EasyEffects, Steam) have
  menus, so left/right click look identical in manual testing; the `Activate()`
  path exists but isn't exercised without a menu-less item.
- **`popup_surface.c` caches the last draw callback and self-triggers the first
  paint from its own `configure` handler** (mirroring how `bar_surface.c`'s
  `configure` handler calls `bar_surface_mark_dirty()`) — this was a real bug
  during development, not a hypothetical: a popup's *first* `popup_surface_render_now()`
  call happens synchronously from a click handler, long before the compositor's
  first `configure` has actually been processed by this process's own Wayland
  dispatch (which only happens later, back in `main.c`'s event loop) — without
  the caching, that first call would silently no-op and *nothing* would ever
  repaint it afterward, since nothing else was watching for the surface to
  become ready.
- **The context menu's `top_margin` subtracts `BAR.height` from the icon's
  bottom edge, not just `icon_y + icon_h`** (`tray_context_menu_open()` in
  `modules/tray/tray_context_menu.c`). The layer-shell spec text describes
  `set_margin`'s distance as measured from the raw output edge, which by that
  reading `icon_y + icon_h` alone should already satisfy — but the menu
  rendered one full bar-height too low without the subtraction. The working
  theory (reasoned from the spec text plus the suspiciously round one-bar-height
  size of the gap, not independently confirmed against Hyprland's own source)
  is that Hyprland measures a layer surface's margin from the edge of its
  *already-reserved exclusive-zone region* rather than the output edge, when
  the margin is set by the same client that owns that reservation — i.e. the
  bar's own 20px top-anchored exclusive zone was being added a second time on
  top of the explicit margin. Fixed by subtracting `BAR.height` back out.
- **The context menu's dismiss-on-outside-click uses one full-screen, fully
  transparent, `LAYER_TOP` "click-catcher" surface *per currently-known
  output*** — not just the output the menu is on. The reference's own dismiss
  mechanism (`HyprlandFocusGrab`) captures compositor-wide; this machine has 2
  monitors, so a single-output catcher would leave the menu stuck open if the
  user clicked the other monitor while it was open — confirmed as a real gap
  worth avoiding given the actual hardware this runs on. The menu popup itself
  is created on `LAYER_OVERLAY` (strictly above the catchers' `LAYER_TOP`) so it
  reliably receives its own clicks first, without depending on same-layer
  creation-order stacking assumptions.

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
- **No popups/tooltips backed by real `xdg_popup`** — the tray's context menu
  uses a second `zwlr_layer_surface_v1` (`bar/popup_surface.c`) positioned via
  anchor+margin math instead, a deliberate choice to avoid implementing
  `xdg_wm_base`/`xdg_positioner` from scratch for one feature (see "D-Bus /
  system tray" above). `xdg-shell.xml` is still linked only for its
  `xdg_popup_interface` symbol (see the Wayland plumbing gotcha above), not
  because anything actually creates an `xdg_popup`.
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
- **Tray: no fallback if customBar's own Watcher registration itself fails**
  (e.g. `sd_bus_add_object_vtable()` erroring after the bus name was already
  claimed — logged to stderr, name released, falls back to Host mode) *and*
  no other Watcher happens to be running either — the tray then stays empty
  for the run, same underlying "nothing to connect to" failure mode as before,
  just less likely to actually occur now that customBar provides its own
  Watcher by default.
- **Tray: no submenu/nested-menu support** — `GetLayout`'s recursion depth is
  capped at 1, matching the reference's own flat rendering.
- **Tray: no tooltip support** — the SNI `ToolTip` property is never fetched.
- **Tray: no scroll-wheel `Scroll()` support** — `wl_pointer` axis events are
  already no-op stubs project-wide, not new to the tray.
- **Tray: no context-menu reflow** if it would overflow past the output's left
  edge (clamped, not repositioned) — also true of the reference.
- **Tray: no keyboard dismiss (Escape)** for the context menu — click-outside
  only, consistent with no keyboard handling existing anywhere in the project.
- **Tray: at most one context menu open at a time** — a deliberate v1
  simplification (`tray_context_menu.c`'s state is a single static singleton).

## Scope / roadmap

Implemented: bar shell (per-output layer-surface, double-buffered shm rendering),
workspace indicator (kanji pills, occupied/focused logic, click-to-focus), clock,
system tray (SNI item tracking, icon rendering, DBusMenu context menu, click routing).

Not yet implemented (planned for later passes, in no particular order): volume, mic,
network, media player (MPRIS), CPU/GPU/memory monitor, power-menu overlay. When
adding one, follow the existing module shape: a `modules/<name>/` directory, a
render function taking `(cairo_t*, ..., x, box_top, box_height)` and returning the
next x coordinate, and — if it needs live data — either a Hyprland IPC event hook
(if relevant) or its own poll/event source integrated into `main.c`'s epoll loop
the same way the clock's timerfd, Hyprland's event socket, and the tray's D-Bus
connection are.
