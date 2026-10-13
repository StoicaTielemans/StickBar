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
workspace indicator, clock, system tray, volume/mic, CPU/GPU/memory monitoring, a
media (MPRIS) player widget, a network/Wi-Fi status widget, and a power-menu
overlay. See "Scope / roadmap" below before assuming a module exists.

## Running / testing changes

```sh
meson setup build      # first time only
meson compile -C build
./build/stickBar
```

No test suite — this is a UI skeleton with no headless-testable logic beyond IPC
parsing. Validate changes by running the bar and observing it (it's fine to run
alongside the existing Quickshell bar; see below), plus watching stderr for Wayland
protocol errors or Hyprland IPC failures.

- **Running alongside Quickshell is safe.** stickBar opens its own independent
  Wayland connection and surfaces; it shares no socket, lock file, or singleton
  resource with Quickshell. Both bars are `LAYER_TOP` with `exclusive_zone=20`
  anchored top/left/right, so wlroots (Hyprland) accumulates the zones additively —
  running both stacks them as two 20px strips (confirmed via
  `hyprctl monitors -j`'s `reserved` field going from `[0,20,0,0]` to `[0,40,0,0]`
  while stickBar runs, and back to `[0,20,0,0]` after it exits cleanly).
- **Screenshotting for visual verification**: `grim` and `slurp` are installed.
  `grim -g "X,Y WxH" out.png` grabs a pixel region across the whole virtual screen
  (multi-monitor X offsets are cumulative, e.g. a 1920-wide first monitor puts the
  second monitor's origin at x=1920) — this is how rendering was checked during
  development, since `grim -o <output>` isn't compatible with `-g`.
- `hyprctl monitors -j` / `hyprctl -j workspaces` / `hyprctl -j activeworkspace` are
  useful for cross-referencing what the bar *should* be showing against what it
  actually renders.
- `pactl get-sink-volume @DEFAULT_SINK@` / `get-source-volume @DEFAULT_SOURCE@` /
  `get-{sink,source}-mute` / `list sinks short` / `list sources short` are the
  equivalent cross-reference for the volume/mic modules — and
  `pactl set-sink-mute @DEFAULT_SINK@ 1` / `set-sink-volume @DEFAULT_SINK@ 30%`
  etc. are a convenient way to trigger a live external change and confirm the
  bar picks it up via its PipeWire subscription without needing to click
  anything (this is how the subscribe → cache → redraw path was verified
  during development).
- `free -h` / `top` / `nvidia-smi` (same query used internally --
  `nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total
  --format=csv,noheader`) are the cross-reference for the CPU/GPU/memory
  modules; `awk '/MemTotal/{t=$2} /MemAvailable/{a=$2} END{printf "%.0f\n",
  100*(t-a)/t}' /proc/meminfo` reproduces the memory module's own percent
  calculation exactly, useful for confirming an off-by-something isn't hiding
  in the rounding.
- `busctl --user list | grep mpris` / `busctl --user introspect
  <name> /org/mpris/MediaPlayer2` / `playerctl -a metadata` (if installed)
  are the cross-reference for the media module -- `playerctl play-pause` /
  `playerctl next` / `playerctl previous` against a real running player
  (e.g. a browser tab or Spotify) are a convenient way to trigger a live
  external change and confirm the bar's PropertiesChanged subscription
  picks it up without needing to click anything, same idea as the volume/mic
  cross-reference above.
- `ip route show default` / `cat /proc/net/route` / `cat /proc/net/wireless` /
  `ip -4 addr show dev <iface>` / `cat /sys/class/net/<iface>/{operstate,
  address,mtu,speed,duplex}` are the cross-reference for the network module
  -- confirmed directly against this development machine's own real routing
  table and interface (`enp8s0`, Ethernet) while writing
  `network/network_client.c`'s parsing (e.g. `/proc/net/route`'s gateway hex
  `0100A8C0` reverses to `192.168.0.1`, matching `ip route show default`
  exactly). This machine has no Wi-Fi interface, so the `isWifi`-branching
  code paths (`/proc/net/wireless` detection, the Wi-Fi icon variants, the
  link-speed/duplex fields being *omitted* for Wi-Fi) are reasoned from the
  reference `NetworkMonitor.qml`'s own logic and spec-level understanding of
  `/proc/net/wireless`'s format, **not independently verified against a real
  Wi-Fi adapter** on this machine — worth re-checking first if a bug ever
  shows up specifically on a Wi-Fi-only report. `nmcli device wifi` / `iwctl
  station wlan0 show` (whichever network manager is in use) are the
  equivalent live cross-reference on a machine that does have Wi-Fi.
- `pkill -SIGUSR1 -x stickBar` (with the bar already running) is the
  cross-reference for the power menu's Hyprland-keybind trigger path — the
  same command the Hyprland keybind itself runs (see "Power menu" below for
  the exact `hyprland.conf` line), so this exercises the identical code path
  without needing to actually add the keybind first. Confirmed this pixel-
  exact against a real `grim` screenshot during development: sampling the
  rendered PNG's own RGB values at each button's border landed exactly on
  `COLORS.yellow`/`sky`/`red`'s known hex values, at exactly the expected
  150×150 size with a 24px gap between buttons.
- **Multi-monitor debugging gotcha, hit directly during this module's own
  development**: this machine's two outputs don't necessarily bind in the
  same left-to-right order `hyprctl monitors -j` lists them in (`Output.index`
  is assigned by *Wayland global binding order*, not compositor-reported
  monitor id — see "Core data model" below) -- a synthetic click driven
  through `app.outputs` (e.g. a temporary debug hook bypassing real pointer
  input) can silently target the *second* physical monitor while a
  screenshot taken at `grim -g "0,0 ..."` only shows the first. Always
  correlate a synthetic click's `Output.index`/`Output.width` (log it) against
  which monitor that actually corresponds to on screen before concluding a
  click "didn't do anything."

## Architecture

### File layout

One subdirectory per concern, and one directory per bar widget under `modules/` —
this is a deliberate, explicit choice (see "Conventions" below), not the minimum
viable structure for a project this size.

```
stickBar/
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
    ├── audio/                    PulseAudio-protocol plumbing for volume/mic (see
    │                             "PulseAudio / volume & mic" below for the full
    │                             writeup) -- structurally the audio-domain twin of
    │                             dbus/ for the tray
    │   ├── pulse_client.c        pa_threaded_mainloop connection, live sink/source
    │   │                         cache (fixed-array, like sni_registry.c), volume/
    │   │                         mute/default-device get+set, eventfd notification
    │   ├── device_popup.c        shared sink/source dropdown popup (used by both
    │   │                         modules/volume and modules/mic) -- structurally
    │   │                         the audio-domain twin of tray_context_menu.c
    │   └── mixer_popup.c         per-app volume mixer (middle-click on the volume
    │                             box): one row per PA sink input, with a
    │                             click/drag/scroll slider + mute toggle
    ├── sysmon/                   /proc + nvidia-smi plumbing for CPU/GPU/memory
    │                             (see "System monitor (CPU/GPU/memory)" below) --
    │                             structurally the sysmon-domain twin of dbus/ and
    │                             audio/, but polled on a plain timerfd rather than
    │                             driven by any async connection of its own
    │   ├── sysmon_client.c       /proc/stat, /proc/meminfo, thermal_zone1, cpuinfo,
    │   │                         loadavg, and a synchronous nvidia-smi popen() call,
    │   │                         all re-read on every 5s poll; plain synchronous
    │   │                         accessors, no locking (single-threaded polling,
    │   │                         unlike pulse_client.c)
    │   └── sysmon_info_popup.c   shared *read-only* CPU/GPU/memory info dropdown
    │                             (used by all three of modules/cpu, modules/gpu,
    │                             modules/mem) -- left-anchored, unlike
    │                             tray_context_menu.c's and device_popup.c's own
    │                             right-anchored popups, since these three modules
    │                             live in the bar's left region
    ├── mpris/                    MPRIS (media player) plumbing for the media module
    │                             (see "Media (MPRIS)" below) -- structurally the
    │                             mpris-domain twin of dbus/'s SNI plumbing, reusing
    │                             that same shared sd_bus session-bus connection
    │                             rather than opening a second one
    │   ├── mpris_client.c        tracked-player table (fixed-array, like
    │   │                         sni_registry.c's), GetAll property fetch per
    │   │                         player, PropertiesChanged + NameOwnerChanged
    │   │                         subscriptions, fire-and-forget PlayPause/Next/
    │   │                         Previous calls, on-demand Position polling
    │   └── mpris_art.c           pure mpris:artUrl -> cairo_surface_t* conversion
    │                             via gdk-pixbuf (no D-Bus types in this file,
    │                             deliberately -- same convention dbus/sni_icon.c
    │                             already documents for its own pure-conversion role)
    ├── network/                  /proc + /sys + getifaddrs() plumbing for the
    │                             network module (see "Network / Wi-Fi" below) --
    │                             structurally the network-domain twin of sysmon/,
    │                             polled on that same shared 5s timerfd rather than
    │                             a timer of its own
    │   └── network_client.c      /proc/net/route, /proc/net/wireless,
    │                             /sys/class/net/<iface>/*, /etc/resolv.conf, and a
    │                             native getifaddrs() IPv4 lookup, all re-read on
    │                             every 5s poll; plain synchronous accessors, no
    │                             locking (single-threaded polling, like
    │                             sysmon_client.c, unlike pulse_client.c)
    └── modules/
        ├── workspaces/
        │   └── workspaces.c      occupied/focused set computation, kanji pills,
        │                         hit-rect recording + click handling
        ├── cpu/
        │   └── cpu.c             CPU %+°C module box render + hit-rect + right-
        │                         click-opens-dropdown handling (no left-click)
        ├── gpu/
        │   └── gpu.c             GPU %+°C module box render (NVIDIA only) +
        │                         hit-rect + right-click-opens-dropdown handling
        ├── mem/
        │   └── mem.c             memory % module box render + hit-rect +
        │                         right-click-opens-dropdown handling
        ├── clock/
        │   ├── clock.c           timerfd-driven formatted time string + rendering +
        │   │                     hit-rect + right-click-opens-calendar handling
        │   └── clock_calendar_popup.c  read-only 12-month year-calendar dropdown
        │                         (today highlighted), structurally the clock's own
        │                         single-widget twin of tray_context_menu.c
        ├── volume/
        │   └── volume.c          default-sink module box render + hit-rect +
        │                         click(mute)/scroll(±5%) handling
        ├── mic/
        │   └── mic.c             default-source module box render + hit-rect +
        │                         click(mute)/scroll(±5%) handling
        ├── media/
        │   ├── media.c           active-MPRIS-player module box render (centered
        │   │                     in the bar, unlike every left/right-anchored
        │   │                     module above) + hit-rect + click(play-pause)/
        │   │                     right-click-opens-dropdown handling
        │   └── media_popup.c     dropdown listing every tracked player (not just
        │                         the active one), each row a thumbnail +
        │                         prev/play-pause/next controls + a read-only
        │                         position/duration timeline -- structurally the
        │                         media-domain twin of device_popup.c
        ├── network/
        │   ├── network.c         default-route interface module box render + hit-
        │   │                     rect + right-click-opens-dropdown handling (no
        │   │                     left-click, same shape as cpu/gpu/mem)
        │   └── network_info_popup.c  read-only interface-details dropdown
        │                         (interface/IP/gateway/MAC/DNS/rx-tx) --
        │                         structurally the network-domain twin of
        │                         sysmon_info_popup.c, but right-anchored since
        │                         this module lives in the bar's right region
        ├── tray/
        │   ├── tray.c            icon row render + hit-rect recording + click
        │   │                     routing (Activate vs open-menu)
        │   └── tray_context_menu.c   popup layer-surface + click-catchers +
        │                         hover/click handling for the DBusMenu entries
        └── powermenu/            the logout/restart/shutdown overlay (see "Power
                                  menu" below) -- the bar's own leftmost item
                                  doubles as its click trigger, matching the
                                  reference's own ArchLogoWidget/PowerMenuState
                                  split
            ├── arch_logo.c        leftmost bar item: a bordered-less "cutout"
            │                     box (solid sapphire fill, mantle-colored glyph)
            │                     + hit-rect + click(toggle) handling
            └── power_menu.c       one full-screen zwlr_layer_surface_v1 per
                                  output (LAYER_OVERLAY), each showing its own
                                  centered Logout/Restart/Shutdown row -- opened
                                  by arch_logo.c's click *or* main.c's SIGUSR1
                                  signalfd handler (the Hyprland keybind trigger)
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
  directly into `modules/workspaces/workspaces.h`, `modules/tray/tray.h` /
  `modules/tray/tray_context_menu.h`, `modules/volume/volume.h`,
  `modules/mic/mic.h`, `modules/cpu/cpu.h`, `modules/gpu/gpu.h`,
  `modules/mem/mem.h`, `modules/clock/clock.h` /
  `modules/clock/clock_calendar_popup.h`, `modules/media/media.h` /
  `modules/media/media_popup.h`, `modules/network/network.h` /
  `modules/network/network_info_popup.h`,
  `modules/powermenu/arch_logo.h` / `modules/powermenu/power_menu.h`,
  `audio/device_popup.h`, and `sysmon/sysmon_info_popup.h` for click/scroll
  routing — a flat list of explicit call sites, not a generic dispatch table
  (see "D-Bus / system tray" below for why that still isn't worth building;
  now at N=11 clickable bar-surface modules plus 7 popup kinds, still cheap
  enough as a flat list that the indirection wouldn't pay for itself).
- **`AppContext.pointer_over` vs `pointer_over_surface`**: `pointer_over_surface`
  is the raw `wl_surface*` currently under the pointer, always kept up to date.
  `pointer_over` is the `Output*` it resolves to *if* it's one of the bar's own
  surfaces — `NULL` for any popup or click-catcher surface, since those are
  never stored on any `Output` (only `bar_surface.c`-owned surfaces are).
  `pointer.c`'s button handler checks each popup kind's surfaces in turn (the
  power menu's, the tray context menu's, the audio device popup's, the
  sysmon info popup's, the media popup's, then the network info popup's —
  each via its own `_is_menu_surface()` / `_is_catcher_surface()` pair;
  since every popup kind's surfaces are disjoint from every other's, the
  order among them doesn't actually matter functionally), *then* falls back
  to the `Output`-keyed path — a popup/catcher surface never matches an
  `Output`, so checking the popup kinds first is required, not just tidier.

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
- **stickBar can act as either the StatusNotifierWatcher itself or a Host
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
  live by running with Quickshell stopped entirely: stickBar claimed
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
    *every* bus-name ownership change system-wide, not just ones stickBar
    tracks — D-Bus match rules can't filter down to "only names I've
    registered items under," so every firing is checked against the registry).
    An empty `new_owner` means that name's previous owner disconnected;
    `sni_registry_remove_by_bus_name()` then removes every item registered
    under that bus name (a single client registering more than one item is
    rare but possible, so this isn't just `sni_registry_remove()` on one
    (bus_name, path) pair). Without this, closing an app would leave its icon
    stuck in the tray forever with no other signal ever telling stickBar it's
    gone — this path has no precedent elsewhere in the codebase to lean on and
    was the one part of this feature confirmed only via a live
    start-app/quit-app/watch-icon-disappear test, not by reasoning about the
    spec text.
  - **Not implementing the Watcher role was the prior limitation this removes.**
    A machine with genuinely nothing providing *any* Watcher (neither
    stickBar's own service nor a fallback like Quickshell/KDE/snixembed)
    would previously have left the tray silently empty forever; now stickBar
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
    with Quickshell down (see below) and stickBar as its own Watcher,
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

## PulseAudio / volume & mic

The volume (default sink) and mic (default source) modules talk to
`pipewire-pulse` — PipeWire's PulseAudio-protocol-compatible server, which is
what's actually running on this machine (`pactl info` reports `Server Name:
PulseAudio (on PipeWire 1.6.8)`) — via **libpulse**, not raw `libpipewire`.

- **Why libpulse instead of the native PipeWire client API**: `libpipewire`'s
  own client API is asynchronous node/param-negotiation machinery designed for
  real audio graph participants (media players, effects); getting a "list of
  sinks with a description and volume" out of it is substantially more
  ceremony than the same query via libpulse's introspection API
  (`pa_context_get_sink_info_list()` and friends), and `pipewire-pulse` makes
  that simpler API available for free on any system already running PipeWire
  (which is effectively every current desktop, this one included). This is
  also the same approach other minimal C status bars (e.g. waybar's
  `pulseaudio` module) take for the identical reason — not a novel choice.
- **`audio/pulse_client.c` runs libpulse's own `pa_threaded_mainloop`**, a
  background thread libpulse provides specifically for embedding into an app
  that doesn't want to hand control of its main thread to `pa_mainloop`'s
  blocking `iterate()` loop — a deliberate deviation from every other
  integration in this project (Wayland, Hyprland, D-Bus, the clock timer),
  which all instead implement a "give me your fds, I'll drive you from my own
  external epoll loop" adapter with no extra thread. That pattern *is*
  possible for libpulse too (`pa_mainloop_api` is an abstract vtable a caller
  can implement itself, exactly like this project already does implicitly for
  Wayland's `prepare_read`/`read_events` dance and explicitly for sd-bus's
  fd+timerfd integration in `dbus/dbus_loop.c`) — but hand-rolling one
  correctly means implementing arbitrary-count I/O watches, arbitrary-count
  timers, and "deferred" (run-every-iteration-until-disabled) callbacks from
  scratch, with subtle failure modes if any of the three is wrong. Given
  `pa_threaded_mainloop` is libpulse's own tested, correct implementation of
  exactly that adapter, reusing it was judged the better trade for this
  module specifically, at the cost of being the one part of this codebase
  that isn't single-threaded. All cross-thread interaction is confined to
  `audio/pulse_client.c` — every other file, including `audio/device_popup.c`
  and the volume/mic modules, only ever calls its plain synchronous-looking
  accessor functions, with no thread-safety concerns of their own.
- **Locking**: `pa_threaded_mainloop_lock()`/`_unlock()` (the mainloop's own
  mutex) is reused as a general-purpose lock for the whole local sink/source
  cache too, not just for calls into libpulse — every accessor in
  `pulse_client.c` (even a plain read like `pulse_node_muted()`) takes this
  lock. This is safe and sufficient because PA callbacks (which mutate the
  cache) are documented to always run on the mainloop thread *with this same
  lock already held*, so there's exactly one lock protecting exactly one
  piece of shared state, not two subsystems needing separate synchronization.
- **Startup blocks until the connection is definitively ready or definitively
  failed** (`pulse_client_init()`'s `pa_threaded_mainloop_wait()` loop) —
  libpulse's own documented pattern for synchronizing an owning thread against
  a `pa_threaded_mainloop`, and the same tradeoff `wayland_connect()` already
  makes with its two blocking `wl_display_roundtrip()` calls: better to know
  immediately (and log) that volume/mic controls are unavailable than to
  discover it several seconds later when no redraw ever arrives.
- **Change notification is a single eventfd** (`pulse_client_notify_fd()`),
  written from inside a PA callback (safe without the mainloop lock — it
  doesn't touch any of libpulse's own data structures) any time the cache
  changes, and drained by `pulse_client_pump()` on the *main* epoll loop —
  same "external notification fd + drain-then-act" shape as
  `dbus/dbus_loop.c`'s timeout-timerfd, just simpler (one fd, not a
  timerfd needing rearming).
- **The cache mirrors sinks/sources by stable `name`, not by PA `index`**,
  for matching against `pa_server_info.default_sink_name`/
  `default_source_name` — PA's numeric `index` isn't guaranteed stable across
  a device's full lifetime the way its string `name` is (this is also exactly
  why `sni_watcher.c`'s SNI item tracking keys off `(bus_name, path)` strings
  rather than any numeric id). `PA_SUBSCRIPTION_EVENT_SERVER` (fired when the
  *default* device changes, not when a device's own properties change) is
  what triggers a `pa_context_get_server_info()` re-fetch of those two names.
- **Volume percent conversion**: `pa_cvolume_avg(&volume) * 100 / PA_VOLUME_NORM`
  (`PA_VOLUME_NORM` = 65536 = 100%), and the reverse for setting — a uniform
  per-channel volume is written back via `pa_cvolume_set(&cv, channels, vol)`
  using the channel count cached from that device's own last-seen
  `pa_cvolume`, not hardcoded to 2 (stereo), though 2 is used as a fallback if
  a device's channel count was never seen yet. `PULSE_MAX_VOLUME_PERCENT` is
  200 (not 100) and `PULSE_VOLUME_STEP_PERCENT` is 5, both matching the
  reference `AudioMonitor.qml`'s own `maxVolume: 2.0` / `step: 0.05` exactly.
- **`audio/device_popup.c` is one shared singleton for both the sink and
  source dropdowns** (`is_sink` selects which), structurally mirroring
  `modules/tray/tray_context_menu.c`'s popup/click-catcher/outside-click-
  dismiss pattern almost exactly (same per-output full-screen transparent
  `LAYER_TOP` catchers, same `LAYER_OVERLAY` menu-above-catchers ordering, same
  "at most one open at a time" rule) — but with one real difference: unlike
  the tray's DBusMenu entries (a one-shot async D-Bus fetch, so
  `tray_context_menu.c` snapshots them into its own array at open time),
  `pulse_client.c`'s sink/source lists are already a live, always-current
  local cache with no fetch latency, so `device_popup.c`'s `compute_layout()`
  just re-reads that cache directly and can be safely re-run on *every* live
  update while the popup is open (`device_popup_refresh_if_open()`, called
  from `main.c` after every `pulse_client_pump()`) — not only once at open
  like the tray menu's entry list.
  - **Each popup kind's per-row status icon deliberately differs**, matching
    the reference's own two *different* `muteIcon(node)` functions rather than
    reusing one: `AudioDevicesPopup.qml`'s is the same 4-level
    muted/low/mid/high icon as `VolumeWidget.qml`'s own; `MicDevicesPopup.qml`'s
    is the simple 2-state mic icon. `device_popup.c`'s
    `row_status_icon_glyph(is_sink, node)` branches on `is_sink` for exactly
    this reason — not an oversight if the two ever look asymmetric in a diff.
  - **Description truncation is byte-based, not UTF-8-codepoint-aware**
    (`truncate_to_width()`) — deliberately, not an oversight: every real
    device description seen from this project's own actual hardware during
    development (e.g. `"Burr-Brown from TI USB Audio CODEC Analog Stereo"`,
    `"Yeti X Analog Stereo"`) is plain ASCII, matching how PulseAudio/PipeWire
    device descriptions are conventionally generated from ALSA card/device
    names in practice.
  - **Positioned right-anchored under its trigger's right edge** (same
    anchor+margin math, including the `BAR.height` correction, as
    `tray_context_menu.c` — see that section above) — a deliberate,
    documented deviation from the reference's own left-aligned-under-widget
    `PopupWindow` positioning, chosen because both the volume/mic modules and
    the tray already live in the bar's right region, so reusing this
    already-correct math was preferred over introducing a second, untested
    anchor scheme for one pair of widgets.
- **Left-click mutes, right-click opens the dropdown, scroll adjusts volume**
  — a deliberate, documented deviation from the reference `VolumeWidget.qml`/
  `MicWidget.qml`, which use hover (with a debounce timer) to show the
  dropdown and reserve click exclusively for mute. This project has no
  per-widget hover/timer infrastructure yet (`wayland/pointer.c` only tracks
  which single surface the pointer is currently over, not hover-duration or
  per-module enter/leave callbacks), and building one purely for this pair of
  widgets was judged not worth it when right-click was already free (nothing
  else in this bar uses it) and is itself a conventional way to expose a
  secondary action alongside a primary click, mirroring how the tray already
  distinguishes `Activate()` from opening its own context menu.
- **Middle-click on the volume box opens a per-application mixer**
  (`audio/mixer_popup.c`, no reference-bar equivalent — added per explicit
  user request). `pulse_client.c` additionally tracks PA *sink inputs* (one
  per app playback stream, `PA_SUBSCRIPTION_MASK_SINK_INPUT`) in a third
  fixed array (`MAX_PULSE_STREAMS` = 32), named by `application.name` with
  the stream's own name as fallback. Each mixer row: app name, a 0–100%
  slider (click to jump, drag to adjust, scroll for ±5% — scroll can still
  push past 100% up to the shared 200% cap), the percent, and a mute-toggle
  icon. Mic-side source outputs (apps *recording*) aren't listed.
  - **Drag relies on Wayland's implicit pointer grab**: while the button is
    held, motion keeps arriving on the mixer surface even outside it, so
    `wayland/pointer.c` routes button *releases* (previously ignored
    entirely) to `mixer_popup_handle_release()` to end the drag. Middle-click
    is filtered in `pointer_button()` to reach only `volume_handle_click()` —
    several other handlers treat "any non-left button" as their secondary
    action (the tray's has-menu rule) and would misfire otherwise.
  - **Live resize is done by recreating the menu surface, not by
    `set_size()` on the existing one.** Apps start/stop streams far more
    often than devices appear, so unlike `device_popup.c` this popup must
    change height while open. A `zwlr_layer_surface_v1_set_size()` + commit
    was tried first and Hyprland unmapped the popup on shrink (no `closed`
    event, no protocol error, it just vanished) — confirmed live via `grim`
    with `paplay` streams starting/ending; destroy-and-recreate (the same
    path every popup open already uses) was then confirmed working for
    grow → shrink → grow. An in-progress drag loses its grab across a
    recreate; the next release still ends it.
  - The mixer and the sink device dropdown close each other on open (both
    hang off the same box at the same spot).
- **Scroll direction convention** (`wayland/pointer.c`'s
  `pointer_axis_discrete()`): scrolling up decreases the wheel's `discrete`
  value below zero and increases volume; scrolling down does the reverse —
  reasoned from `wayland.xml`'s doc comment (axis `value`/`discrete` share "a
  coordinate space identical to those of motion events," i.e. positive = the
  same direction as scrolling a page down) plus the conventional Linux
  volume-scroll mapping, **not independently verified against this machine's
  actual mouse** — no working synthetic-input setup was available in the
  environment this was developed in (no `ydotoold` running, no group
  permission on `/dev/uinput`) to test scroll direction the way clicks and the
  tray's margin fix were tested. Trivial one-line flip (swap the ternary in
  `pointer_axis_discrete()`) if it turns out backwards in practice.
- **`wl_pointer`'s `axis_discrete` event (not the newer, higher-resolution
  `axis_value120`) drives scroll-to-adjust** — `axis_value120` was added in
  wl_pointer version 8, but `wayland/registry.c`'s `SEAT_VERSION` is 7 (the
  pointer object is only ever created at the bound seat's version), so
  `axis_value120` is simply never sent to this process regardless of the
  compositor's own capabilities. `axis_discrete` (added in v5) is what's
  actually available, and its magnitude is deliberately ignored (treated as
  ±1 regardless of the reported value) — one call is meant to represent
  exactly one physical wheel notch, matching the "steps of 5%" behavior that
  was asked for.

## System monitor (CPU/GPU/memory)

The reference bar has one combined `SystemMonitorWidget`/`SystemMonitor.qml`
showing CPU/GPU/memory stats together with a single hover tooltip. Per
explicit user request, this project splits it into **three independent
modules** (`modules/cpu`, `modules/gpu`, `modules/mem`) — separate boxes,
separate accent colors (peach/green/yellow — chosen from the unused remainder
of the Catppuccin palette; pink is already workspaces', mauve is the clock's,
sky is volume's, blue is mic's), and each with its **own** right-click
dropdown, rather than one shared tooltip for all three.

- **Polled on a plain 5-second timerfd `main.c` owns directly** (same shape as
  the clock's own 1-second timer, not the "give me your fd, I'll drive your
  async connection" pattern `dbus/dbus_loop.c` and `audio/pulse_client.c`
  use) — matches the reference `SystemMonitor.qml`'s own `Timer { interval:
  5000 }` exactly. `sysmon/sysmon_client.c` has no persistent connection or
  background thread of any kind; every poll just re-reads `/proc/stat`,
  `/proc/meminfo`, `/sys/class/thermal/thermal_zone1/temp`, `/proc/cpuinfo`,
  and `/proc/loadavg` from scratch (all cheap: small pseudo-files, no syscalls
  beyond open/read/close) plus one `nvidia-smi` subprocess call (see below).
  No locking anywhere in this file, unlike `pulse_client.c` — everything runs
  on the main thread, so there's no concurrent writer to guard against.
- **`nvidia-smi` is invoked synchronously via `popen()`/`pclose()`, blocking
  the whole event loop for its duration** — a deliberate, measured tradeoff,
  not an oversight. Timed directly on this machine: ~15-45ms per call. Once
  every 5 seconds, that's roughly a 0.3-0.9% duty cycle of blocked time, and
  it only delays *this* poll tick's redraw plus any input events that happen
  to arrive in that exact window — not a per-frame or per-click cost. The
  alternative (spawning it as a genuinely async subprocess with its stdout
  pipe registered on `main.c`'s epoll, reaped via a `SIGCHLD` handler or a
  `waitpid(WNOHANG)` poll) would need a new "transient fd added and removed
  every poll cycle" pattern nothing else in this codebase has, for a benefit
  (avoiding a sub-50ms block once per 5s) judged not worth that added
  complexity here. If `nvidia-smi` is missing or fails (no NVIDIA GPU, driver
  not loaded), `sysmon_gpu_available()` stays false, the GPU module shows
  `--` instead of a percent, and its dropdown shows an explicit "No NVIDIA GPU
  detected" message instead of stale/zero-looking numbers — the same
  "explicit unavailable state, not a misleading zero" instinct behind
  `pulse_client.c`'s accessors returning safe defaults when disconnected.
- **CPU-jiffies delta computation matches the reference's own
  `percentFromJiffies()` exactly**: `/proc/stat`'s `cpuN` lines are parsed
  every poll, and each line's `(idle, total)` pair is diffed against the
  *previous* poll's sample for that same core (aggregate `cpu` line tracked
  separately from per-core `cpu0`, `cpu1`, ... lines) to get a percentage —
  which is why the very first poll after `sysmon_client_init()` always
  reports 0% (no prior sample yet to diff against), self-correcting on the
  next poll 5 seconds later. Not a bug; matches the reference's own `prev ?
  percent : 0` behavior.
- **`bar/popup_surface.c`'s `popup_surface_create()` gained a `bottom_margin`/
  `left_margin` pair it didn't have before this module** (previously only
  `top_margin`/`right_margin`, with bottom/left implicitly always 0) — a real
  bug was caught here during development, not a hypothetical: the CPU/GPU/
  memory dropdowns were first written copying `tray_context_menu.c`'s/
  `device_popup.c`'s TOP+RIGHT anchor-under-the-trigger math verbatim, which
  is correct for those two (both live in the bar's *right* region, near the
  screen's right edge, so growing the popup leftward from the trigger's right
  edge stays on-screen) but wrong here — CPU/GPU/memory live in the bar's
  *left* region near x=0, so a right-anchored popup grew leftward off the
  left edge of the screen and was invisible. The user caught this directly
  ("the popup get cut off the left side of the screen"). Fixed by generalizing
  `popup_surface_create()`'s signature to accept all four margins (updating
  its two existing callers to pass `0, 0` for the now-explicit bottom/left
  params) and having `sysmon_info_popup.c` anchor TOP+LEFT with an explicit
  `left_margin = box_x` instead — grows rightward from the trigger's left
  edge, which is the correct direction for a left-region trigger. No
  overflow-past-the-*right*-edge clamping was added (unlike the right-anchored
  popups' existing left-edge clamp) since no real caller today is close enough
  to the right edge to hit it — see "Known limitations" below.
- **Right-click opens the dropdown; left-click does nothing** — unlike
  volume/mic (where left-click has an obvious action, muting), there's no
  equivalent single "primary action" for a system-monitor widget, so unlike
  every other clickable module in this bar, `cpu_handle_click()`/
  `gpu_handle_click()`/`mem_handle_click()` only ever react to `BTN_RIGHT`.
  This also matches the reference's own `SystemMonitorWidget.qml`, which has
  no `onClicked` handler at all (hover-only) — right-click here is this
  project's now-established translation of "hover to see more" into a
  click-based interaction model (same translation already made for volume/
  mic's device dropdown), just without a competing left-click action to work
  around.
- **Debugging note from this module's own development**: a temporary SIGUSR1-
  driven debug hook (used to manually verify the popup position fix above,
  since this environment still has no working synthetic-input setup) sent its
  synthetic click to `app.outputs` (the first-bound `Output`) without checking
  *which* physical monitor that was — on this 2-monitor machine, that happened
  to be the second monitor (`Output.index == 1`, `HDMI-A-2`), while a
  `grim -g "0,0 ..."` screenshot only ever captures the first. The click was
  silently landing on and missing a bar the screenshot never showed, which
  looked identical to "the click did nothing." Resolved by logging
  `Output.index` from inside the click handler and screenshotting the matching
  monitor's actual x-offset instead. See "Running / testing changes" above.

## Clock year-calendar popup

The reference bar's `ClockWidget.qml` shows a small current-week strip on
hover and a full 12-month year-calendar grid (`ClockYearPopup.qml`) on
left-click, toggled via `yearOpen = !yearOpen`. This project reproduces only
the year-calendar grid, and reachable via **right**-click, not left or hover.

- **Right-click, not left-click-toggle or hover, per this project's own
  already-established convention** (see "System monitor (CPU/GPU/memory)"
  above): there's no hover/timer infrastructure in `wayland/pointer.c` (it
  only tracks which single surface the pointer currently sits over, no
  hover-duration or per-module enter/leave callbacks), and building one purely
  to reproduce the reference's week-strip hover preview wasn't judged worth it
  for a single widget — the same tradeoff already made for volume/mic's device
  dropdown and cpu/gpu/mem's info dropdowns. The week-strip preview itself is
  therefore not reproduced at all, only the year grid; a right-click toggles
  it open/closed, mirroring the reference's own `yearOpen` toggle semantics
  exactly, just on a different button.
- **`modules/clock/clock_calendar_popup.c` is structurally
  `tray_context_menu.c`'s single-widget twin**: same popup/click-catcher/
  outside-click-dismiss shape (one full-screen transparent `LAYER_TOP` catcher
  per currently-known output, menu itself on `LAYER_OVERLAY`), same
  right-anchored-under-the-trigger anchor+margin math (including the
  `BAR.height` exclusive-zone correction — see "D-Bus / system tray" above)
  since the clock lives in the bar's right region like the tray, not the left
  like CPU/GPU/memory.
- **The year/month/day grid is computed from scratch in C** (no equivalent of
  `CalendarUtils.js` existed yet): `days_in_month()`/`first_weekday_mon0()`
  both go through `mktime()`'s own date normalization (asking for "day 0 of
  the following month" rather than a hand-rolled leap-year table, and reading
  back `tm_wday` for the 1st of the month) rather than reimplementing calendar
  math by hand — noon (`tm_hour = 12`) is used for both to stay clear of any
  DST transition exactly at midnight.
- **Every month card is padded to a uniform 6 week-rows**, unlike the
  reference's own per-month-height `Repeater` (which lets Qt's `GridLayout`
  leave blank space under a shorter card in the same row when neighboring
  cards in that row need more weeks) — a deliberate simplification so all 12
  cards line up into a perfectly uniform grid via plain arithmetic, without
  needing to replicate Qt's row-height-negotiation behavior.
- **The displayed year/today's date is snapshotted once at popup-open time**,
  not live-updated while open — same "snapshot at open, don't bother
  keeping fresh for a short-lived popup" choice already made for
  `tray_context_menu.c`'s DBusMenu entries (unlike `device_popup.c`'s
  sink/source rows, which *are* kept live because PulseAudio's cache updates
  are already free — see "PulseAudio / volume & mic" above for that
  distinction).
- **Three new shared font-size tokens were added to `config/config.h`'s
  `FontConfig`** (`xs`=8, `md`=11, `title`=16), ported directly from the
  reference `config.js`'s own `font.sizes.{xs,md,title}` values — these are
  generic enough (fine print, secondary emphasis, large titles) to be genuine
  shared tokens rather than one-off local tuning, unlike e.g. the workspace
  pill's own local padding constants.

## Media (MPRIS)

The reference bar's `MediaWidget.qml` lives centered in the bar's own `centerLayout`
(the only widget there) and wraps `Quickshell.Services.Mpris` via a `MediaPlayer.qml`
singleton. This project's `mpris/mpris_client.c` talks to the same
`org.mpris.MediaPlayer2.*` D-Bus protocol directly via sd-bus, reusing the tray's own
shared session-bus connection (`dbus/dbus_loop.c`) rather than opening a second one —
structurally the mpris-domain twin of `dbus/`'s SNI plumbing (`mpris_client.c`'s
tracked-player table mirrors `sni_registry.c`'s `SniItem` array almost exactly: a
fixed, non-relocated array so a slot's address stays valid as a `sd_bus_match_signal()`
userdata pointer, `in_use`-gated iteration, `_count()`/`_get(index)` accessors).

- **Player discovery**: `ListNames` on `org.freedesktop.DBus` at startup, filtered to
  the `org.mpris.MediaPlayer2.` prefix; a global `NameOwnerChanged` subscription
  (filtered *inside* the callback by that same prefix, not via a raw match-string
  API this codebase doesn't otherwise use) catches players appearing/disappearing
  afterward — same "global match, filter inside the callback" shape
  `dbus/sni_watcher_service.c`'s own `on_name_owner_changed()` already uses for the
  identical reason (D-Bus match rules can't filter an arbitrary name *prefix*
  without that raw-string API). `playerctld` is excluded by a substring check on the
  bus name, matching `MediaPlayer.qml`'s own filter — it's a shadow/mirror service
  that duplicates whatever's actually playing under its own MPRIS name, so including
  it would show every real player twice.
- **Every MPRIS player lives at the same fixed object path**
  (`/org/mpris/MediaPlayer2`), unlike SNI items (which each declare their own path) —
  no per-item path discovery is needed here at all.
  `mpris_client.c` subscribes to that path's `org.freedesktop.DBus.Properties`
  interface with `member=NULL` (matches every signal from that interface, which in
  practice is only ever `PropertiesChanged`) — same "match broadly, dispatch by
  member name inside the callback" convention `sni_registry.c`'s own per-item
  subscription already uses.
- **On `PropertiesChanged`, the whole `Player`-interface property set is re-fetched
  via a fresh `GetAll` call, rather than parsing the signal's own incremental
  changed-properties payload.** Simpler and more robust than tracking partial
  updates, at the cost of one extra round trip per change — the same tradeoff
  `sni_registry.c`'s `refresh_icon()` already makes for `NewIcon` (re-fetch the one
  property that matters rather than parse what the signal handed over). Acceptable
  here since `PropertiesChanged` fires relatively rarely (track changes, play/pause
  toggles), not on a hot path.
- **`Identity` (the player's own display name, used as a title fallback) lives on the
  root `org.mpris.MediaPlayer2` interface, not `Player`** — fetched as a second,
  separate `sd_bus_get_property()` call after the `Player` GetAll. Best-effort: a
  player that fails to answer it just keeps whatever identity string (possibly
  empty) it already had, rather than treating the whole property fetch as failed.
- **`xesam:artist` is an array of strings per spec** (a track can have multiple
  credited artists) — only the first is kept, matching `trackText()`'s own
  single-artist display. The D-Bus reader still drains every remaining array entry
  before calling `sd_bus_message_exit_container()`, same reasoning
  `sni_registry.c`'s `convert_best_pixmap_from_message()` already documents for
  draining every `IconPixmap` entry rather than stopping early once the best one is
  found — leaving unread bytes in a `sd_bus_message` before exiting its container is
  not safe to skip.
- **Fire-and-forget method calls** (`PlayPause`/`Next`/`Previous`, via
  `mpris_player_toggle_playing()`/`_next()`/`_previous()`): `sd_bus_send()`, not
  `sd_bus_call_method()`, for the same reason `dbus/sni_registry.c`'s
  `sni_item_activate()` already documents — this runs from a UI click handler inside
  the bar's single-threaded event loop, and blocking on a remote player's reply
  would freeze the whole bar until it responds. `mpris_player_toggle_playing()`
  always sends `PlayPause` unconditionally rather than replicating the reference
  `togglePlaying()`'s `canTogglePlaying`-gated `Play()`/`Pause()` fallback —
  `PlayPause` is spec-optional but implemented by essentially every real player, and
  the extra capability branching wasn't judged worth it for v1.
- **The bar widget (`modules/media/media.c`) is horizontally centered in the bar**,
  the only module in this codebase that isn't anchored to either the left or right
  region — matches the reference's own `centerLayout` /
  `anchors.horizontalCenter: parent.horizontalCenter`. It shows the *active* player
  (`mpris_active_player()`: first currently-`Playing` tracked player, or the first
  tracked player at all, or nothing) and collapses to zero width when there's no
  player at all, matching `MediaWidget.qml`'s own `visible: player !== null`.
- **Left-click toggles play/pause; right-click opens a dropdown** listing every
  tracked player (not just the active one), each row with prev/play-pause/next
  controls — this project's now-established right-click-opens-dropdown convention
  (see volume/mic/cpu/gpu/mem/clock above), replacing the reference's
  hover-triggered `MediaPopup` since `wayland/pointer.c` has no hover-duration/timer
  infrastructure (same reasoning already given for every other popup in this bar).
  `modules/media/media_popup.c` is structurally the media-domain twin of
  `audio/device_popup.c`: same popup/click-catcher/outside-click-dismiss shape, same
  "recompute layout on every relevant live update while open, not just at open time"
  model (`media_popup_refresh_if_open()`, called from `main.c` after every D-Bus
  pump — cheap, since it's a no-op unless the popup is actually open) as
  `device_popup.c`'s own `device_popup_refresh_if_open()`.
  - **Anchored TOP+LEFT under the trigger's left edge**, like
    `sysmon/sysmon_info_popup.c` rather than the tray's/audio popup's own
    TOP+RIGHT anchoring — chosen because the media widget lives in the bar's
    *center* region rather than flush against either edge, so growing the popup
    rightward from the trigger's left edge was judged the more reasonable default.
    No overflow-past-either-edge clamping is added, same documented limitation as
    every other popup in this bar.
- **Each dropdown row also shows a thumbnail and a read-only position/duration
  timeline**, matching the reference `MediaPopup.qml`'s own layout fairly closely
  (a left-hand `Image` beside a title + timeline + controls stack) rather than the
  bar-widget-derived text-only row this section originally shipped with.
  - **Thumbnail (`mpris/mpris_art.c`)**: MPRIS's `mpris:artUrl` is only fetched for
    `file://` URIs, decoded via `gdk-pixbuf-2.0` (a new dependency, added
    specifically for this — cairo alone only decodes PNG natively, which would miss
    most real-world, commonly-JPEG cover art) and converted to a premultiplied
    `CAIRO_FORMAT_ARGB32` surface by hand, the same "no toolkit's own
    cairo-integration helper, do the pixel conversion directly" approach
    `dbus/sni_icon.c` already takes for SNI `IconPixmap` bytes. `http(s)://` and
    other schemes are skipped entirely (returns `NULL`, drawn as a plain
    placeholder square) — this project has no HTTP client, so a streaming
    service's own CDN-hosted art (Spotify's own cover-art URLs, most in-browser
    media) never gets fetched; only locally-cached art (mpv, VLC, Firefox's own
    cached media thumbnails) shows up. Decoding happens once per real track
    change (`mpris/mpris_client.c`'s `parse_metadata()` diffs the new `artUrl`
    string against the cached one before reloading), not on every property
    refresh, so a play/pause toggle (which re-sends the same `Metadata`) doesn't
    re-decode the same image.
  - **Position/duration timeline**: MPRIS's `Position` property isn't
    push-notified via `PropertiesChanged` per spec (unlike everything else this
    popup shows), so it needs actual polling while something's watching it, same
    as the reference `MediaPopup.qml`'s own `Timer { running: root.visible }`
    re-triggering `positionChanged()`. Rather than add a new poll source,
    `media_popup_tick()` piggybacks on `main.c`'s already-ticking 1-second clock
    timerfd (a no-op unless the popup is actually open) and re-fetches every
    visible row's player's `Position` via a synchronous `sd_bus_get_property()`
    call each second. `mpris/mpris_client.c` deliberately does *not* poll
    `Position` on its own regular schedule — only `media_popup_tick()` (and
    `media_popup_open()`, once, for the popup's very first frame) calls
    `mpris_player_refresh_position()`, so a player nobody's looking at isn't
    polled at all. The bar (the "24h sentinel" `mpris:length` heuristic, "m:ss"
    formatting, the 4px filled bar) all mirror `MediaPopup.qml`'s own
    `maxSaneDuration`/`formatTime()`/timeline styling. **Read-only, no
    click-to-seek** — `SetPosition()` needs the track's own `mpris:trackid`
    object path, which isn't cached anywhere (only the fields this popup
    actually displays are), and adding it purely to support seeking wasn't
    judged worth it here.
- **Deliberately out of scope for this pass**: no per-player volume/mute control
  (would need client-side mute-state bookkeeping like the reference's own
  `_mutedVolumes`, judged not essential for v1) and no click-to-seek on the
  timeline (see above). See "Known limitations" below.

## Network / Wi-Fi

The reference bar's `NetworkWidget.qml` + `NetworkTooltip.qml` wrap a
`NetworkMonitor.qml` singleton that reads `/proc`/`/sys` natively (no persistent
connection to NetworkManager/systemd-networkd/iwd or any other network-management
D-Bus service — the reference doesn't talk to one, and this project doesn't either,
for the same reason: everything it displays is derivable from plain files). This
project's `network/network_client.c` is a direct C port of that same file-reading
approach, structurally the network-domain twin of `sysmon/sysmon_client.c` — same
"re-read everything from scratch on every poll, plain synchronous accessors, no
locking" shape, reusing `main.c`'s existing 5-second sysmon timerfd rather than
adding a second one for the same cadence (`NetworkMonitor.qml`'s own `Timer` uses
the identical 5000ms interval).

- **Default-route discovery**: `/proc/net/route`'s `Destination` column is
  `00000000` for exactly the default route; that row's `Iface` and `Gateway`
  columns give the active interface and its gateway. `Gateway`'s 8-hex-digit
  value packs an IPv4 address as 4 raw bytes in the kernel's own wire order —
  confirmed against this development machine's own real routing table (gateway
  hex `0100A8C0` reverses to `192.168.0.1`, matching `ip route show default`
  exactly), not just reasoned from spec text, using the same reversed-byte-pair
  algorithm the reference `hexToIp()` uses, ported to C in `hex_to_ip()`.
- **Wi-Fi vs. Ethernet detection**: `/proc/net/wireless` lists one line per
  wireless interface (after two header lines) — an interface's mere *presence*
  in that file (matched by its name prefixing a line, after trimming leading
  whitespace) means it's a Wi-Fi device; nothing in this project actually parses
  the quality/signal-level columns that follow, matching the reference's own
  `NetworkMonitor.qml`, which reads the same file for the identical yes/no
  purpose and never surfaces signal strength either.
- **IPv4 address lookup uses `getifaddrs()` (POSIX libc), not a shelled-out
  `ip -4 addr show dev <iface>`** — a deliberate improvement over the reference,
  not just a stylistic port choice. `NetworkMonitor.qml` only re-runs its own `ip
  addr` subprocess when the interface actually changes (its own `ifaceChanged`
  gate, since a subprocess spawn is comparatively expensive and the IP rarely
  changes once assigned); `getifaddrs()` is a single syscall over already
  kernel-resident data, cheap enough to just call on every 5-second poll with no
  equivalent change-gating needed, and it sidesteps interpolating a kernel-
  reported interface name into a shell command string entirely (not a real
  attack surface in practice — interface names are kernel/udev-controlled, not
  user input — but a plain libc call needs no such reasoning about safety at
  all). Same "no subprocess for something the OS already gives you a native call
  for" instinct the reference's own `Time.qml` documents for its native
  `new Date()` vs. shelling out to `date`.
- **rx/tx rate is a delta between the two most recent polls**, sampled against
  `CLOCK_MONOTONIC` (not wall-clock time, unlike the reference's own `Date.now()`
  — immune to an NTP time jump mid-session skewing one sample's elapsed-time
  divisor), reset (no prior sample to diff against) whenever the default-route
  interface itself changes — same self-correcting "0 until the next poll"
  behavior `sysmon_client.c`'s own CPU-jiffies delta already has, and the same
  reason a network-interface change (e.g. Ethernet unplugged, Wi-Fi took over)
  shouldn't diff two byte counters that belong to two different interfaces.
- **DNS servers**: every `nameserver` line in `/etc/resolv.conf`, capped at 8
  entries (`MAX_DNS_SERVERS` in `network_client.c`) — comfortably above the 3
  glibc's own resolver actually honors, matching this project's usual bounded-
  array convention rather than a note-worthy real limitation.
- **The bar widget (`modules/network/network.c`) shows the connection-state icon
  plus a live rx/tx rate** (`↓12.3 KB/s ↑4.5 KB/s`, or `Disconnected` when the
  interface is down but still has a default route, matching the reference's own
  label text exactly) — the down/up arrows are plain BMP Unicode (`↓`/`↑`, not
  Nerd Font PUA glyphs) but still routed through `glyph_utf8()` with numeric
  codepoints rather than hand-typed as raw literals, per this project's usual
  glyph convention (see "Conventions" below), even though these two are common
  enough that hand-typing them would likely have been fine in practice.
- **Accent color is `COLORS.teal`, not the reference's own `Config.colors.green`**
  — a forced deviation, not a preference: this project's own earlier CPU/GPU/
  memory pass already claimed `green` for the GPU module (see "System monitor
  (CPU/GPU/memory)" above), which didn't exist as a conflict in the reference
  (its `SystemMonitorWidget.qml` and `NetworkWidget.qml` are visually
  independent widgets that never needed to coordinate on a shared accent-color
  pool the way this project's per-module color assignment does). `teal` was
  picked from the palette's remaining unclaimed colors as the closest visual
  analogue to green still available.
- **No left-click action; right-click opens the info dropdown** — matches
  `modules/cpu/cpu.h`'s own established convention exactly (there's no obvious
  single "primary action" for a status-only widget, unlike volume/mic's
  click-to-mute), and is itself already a deviation from the reference (hover
  tooltip, no click at all) for the same reason every other popup in this bar
  makes that same swap: `wayland/pointer.c` has no hover-duration/timer
  infrastructure.
  `modules/network/network_info_popup.c` is structurally the network-domain twin
  of `sysmon/sysmon_info_popup.c` (single read-only text block, snapshotted at
  open time, not live-refreshed while open — this data only changes once every
  5-second poll tick anyway, so there's no equivalent of `device_popup.c`'s
  live-while-open refresh to bother with) — but anchored **TOP+RIGHT**, unlike
  `sysmon_info_popup.c`'s TOP+LEFT, since the network widget lives in the bar's
  *right* region (between Volume and Clock, matching the reference `Bar.qml`'s
  own `rightLayout` order) rather than the left like CPU/GPU/memory. Body text
  mirrors `NetworkTooltip.qml`'s own `NetworkMonitor.tooltip` → `buildTooltip()`
  exactly, including omitting the `LINK SPEED`/`duplex` line entirely for Wi-Fi
  (those fields don't apply) and the blank-line section breaks between the
  connection-details block, the rate/totals block, and the DNS list.
- **Not independently verified against a real Wi-Fi adapter** — this development
  machine has only Ethernet (`enp8s0`); every Wi-Fi-specific code path (the
  `/proc/net/wireless` detection, the Wi-Fi icon variants, `LINK SPEED`/duplex
  being omitted) is reasoned from the reference's own logic and
  `/proc/net/wireless`'s documented format, not confirmed live the way the
  Ethernet path's route/gateway/IP parsing was. See "Running / testing changes"
  above for the cross-reference commands to use if this ever needs checking on
  Wi-Fi-capable hardware.

## Privacy indicator (mic/webcam/screen-share)

Has no equivalent in the reference Quickshell bar at all — this is a waybar-style
addition, added per explicit user request, showing when any application is
recording the mic, reading the webcam, or capturing the screen, and which
application. Structurally the newest "domain" subsystem alongside `dbus/`,
`audio/`, `sysmon/`, `mpris/`, and `network/`: `pipewire/pipewire_client.c` talks
directly to PipeWire (not via the PulseAudio-compatible protocol
`audio/pulse_client.c` uses for volume/mic control — that protocol has no
equivalent visibility into the raw stream graph or into video streams at all),
and `modules/privacy/privacy.c` + `privacy_info_popup.c` are the bar-facing half.

- **`pipewire_client.c` reuses `pulse_client.c`'s exact shape**: a `pw_thread_loop`
  (PipeWire's own tested analogue of `pa_threaded_mainloop` — an owned background
  thread + lock, chosen over hand-rolling a `pw_loop`-in-epoll adapter for the
  same reasoning `pulse_client.c` already gives for its own choice), one eventfd
  for cross-thread change notification, and the thread-loop's own lock reused as
  a general-purpose mutex over two fixed-size tables (`g_nodes[256]`,
  `g_links[256]`) — same bounded-array convention as `sni_registry.c`'s
  `MAX_TRAY_ITEMS`/`mpris_client.c`'s `MAX_MPRIS_PLAYERS`.
- **Detection is a `pw_registry` global listener**, tracking every `Node` global's
  `media.class` and a resolved display name (`application.name` →
  `node.description` → `node.name` → `media.name` → `"Unknown app"` fallback
  chain), plus every `Link` global's `link.output.node`/`link.input.node` — the
  latter read straight off the link's own registry **global properties**, with
  no need to bind the link object at all (confirmed live via `pw-dump` during
  development: a link's `props` already carries both node ids). A node whose
  `media.class` is `Stream/Input/Audio` or `Stream/Input/Video` is additionally
  bound (`pw_registry_bind` + a `pw_node_events.info` listener) so its live
  `PW_NODE_STATE_RUNNING` state is tracked — a node merely *existing* isn't
  enough to count as "in use" (an app can hold the device open while muted/
  paused), matching this project's general instinct toward explicit-state-over-
  presence already seen in `pulse_client.c`'s own default-device tracking.
- **Mic detection is independently confirmed live**: a `parecord` capture during
  development produced exactly the expected node
  (`media.class=Stream/Input/Audio`, `state=running`,
  `application.name=parecord`), and the full pipeline (icon appears within one
  redraw, right-click dropdown reads `MICROPHONE: parecord`, icon disappears
  when the recording ends) was screenshot-verified end-to-end via `grim`, plus a
  real synthetic right-click via `ydotool`/`ydotoold` (this machine's user
  account has `/dev/uinput` group access via the `input` group, so — unlike
  every previous "no working synthetic-input setup" caveat elsewhere in this
  file — a real click was driven all the way through this module's own
  click-routing and popup-open path, not just reasoned about).
- **Webcam vs. screen-share is an elimination heuristic, NOT independently
  verified** — both show up identically as a `Stream/Input/Video` node with no
  tag on the node itself saying which. `video_node_is_webcam()` in
  `pipewire_client.c` follows the `Link` whose `link.input.node` is this stream
  back to its `link.output.node`, and checks *that* node's own `media.class`: a
  prefix of `Video/Source` (a real v4l2/libcamera device-monitor node) →
  webcam; anything else (including no link yet) → screen-share, by elimination.
  The `Video/Source` convention itself is high-confidence — it's PipeWire's own
  documented device-monitor role naming, and was independently confirmed live
  for the exactly analogous audio case (real ALSA mic devices show up as
  `Audio/Source` nodes, confirmed via `pw-dump` while `parecord` was running) —
  but this machine has no `/dev/video*` device and driving
  `xdg-desktop-portal-hyprland`'s screen-share picker needs interactive UI a
  headless test can't provide, so the actual webcam/screen-share split was
  never exercised against real hardware/a real portal session. **If this is
  ever wrong in practice, `video_node_is_webcam()` is the one function to
  re-check first** — most likely fix would be adjusting which `media.class`
  prefix (or which other property) actually distinguishes a portal-created
  screen-capture producer on the machine where it's wrong.
- **Accent color is `COLORS.maroon`** — confirmed via a full-codebase grep that
  `rosewater`, `flamingo`, `maroon`, and `lavender` were the only Catppuccin
  Mocha palette entries no other module had claimed yet at the time this was
  added; maroon was picked for its "recording/alert" connotation, and doesn't
  collide with `red` (already the power menu's Shutdown-button color).
- **New glyphs** (`config/glyphs.h`): `GLYPH_ICON_WEBCAM` (`0xF05A0`,
  `nf-md-webcam`) and `GLYPH_ICON_SCREENSHARE` (`0xF1483`, `nf-md-monitor_share`),
  both found via a live fontTools cmap search against
  `JetBrainsMonoNerdFont-Regular.ttf` for glyph names containing
  "webcam"/"monitor_share" (same verification convention as every other glyph in
  this file — not guessed from a cheat sheet). Mic reuses the existing
  `GLYPH_ICON_MIC` the volume/mic modules already use.
- **The bar widget (`modules/privacy/privacy.c`) lives in the bar's left region**,
  right after the memory module, and — per explicit user request ("when I don't
  use mic or screenshare I just don't want to see anything") — **collapses to
  zero width when nothing is active**, the same convention
  `modules/media/media.c` uses for "no MPRIS player at all": no box is drawn and
  the render function returns its input `x` unadvanced, which render.c's plain
  `x = module_render(cr, output, x, ...)` chaining tolerates with no special
  case needed. When at least one category is active, the box shows one icon per
  active category (mic/webcam/screen-share, space-joined), all in maroon, no
  percent/text unlike cpu/gpu/mem's own boxes.
- **No left-click action; right-click opens a read-only dropdown** — matches
  cpu/gpu/mem/network's own established convention exactly (no obvious single
  "primary action" for a status-only widget). `modules/privacy/
  privacy_info_popup.c` is structurally sysmon_info_popup.c's near-twin: same
  TOP+LEFT anchor math (including the `BAR.height` correction), same per-output
  click-catcher creation, same read-only "absorb click on menu surface, close on
  catcher click" behavior, snapshotted at click time rather than live-refreshed
  while open (this data only changes on a real PipeWire graph change anyway, and
  a right-click re-snapshots it fresh each time) — the one difference is it has
  only the one caller/accent (`COLORS.maroon`), so unlike
  `sysmon_info_popup_open()` it takes no `accent` parameter. The dropdown body
  groups entries into `MICROPHONE:`/`WEBCAM:`/`SCREEN SHARE:` sections (a
  section header is omitted entirely if empty), each app name on its own
  indented line, built fresh in `privacy_handle_click()` from
  `pipewire_client_entry_count()`/`_entry()`.

## Power menu

The reference bar's logout/restart/shutdown overlay is its own second full shell
"part" (`powermenu/`, alongside `bar/`), triggered by `bar/modules/ArchLogoWidget.qml`
— the bar's own leftmost item, doing double duty as both a widget and a button —
toggling a shared `PowerMenuState` singleton. This project reproduces that same
split: `modules/powermenu/arch_logo.c` is the bar-surface trigger,
`modules/powermenu/power_menu.c` is the overlay itself, and both only ever talk to
each other through `power_menu_toggle()` — matching `PowerMenuState.toggle()`
exactly, with no separate `_open()`/`_close()` pair exposed, since every trigger
this bar has (the button, and the keybind below) always wants toggle semantics.

- **`ArchLogoWidget.qml`'s "cutout" styling is deliberately *not* routed through
  the usual bordered module-box look** every other widget uses: a solid
  `COLORS.sapphire` fill with the Arch glyph rendered in `COLORS.mantle` (the
  bar's own background), no border at all (`module_box_draw_rect()` called with
  `border_width = 0.0`) — so the logo reads as a shape cut out of a solid block,
  not colored text on a dark box. `sapphire` and `red` (used below for the
  Shutdown button) were both still fully unclaimed by any other module's own
  accent color at the time this was added — unlike the network module's own
  forced `teal`-instead-of-`green` substitution (see "Network / Wi-Fi" above),
  no collision to work around here.
- **The Arch logo's box is sized from the glyph's *ink* extent, not the
  reference's own `implicitWidth: label.implicitWidth + 8`** — a real bug
  caught live (the user reported "the arch logo is cut off"), not a
  hypothetical. `module_box_measure_text()`'s underlying
  `pango_layout_get_pixel_size()` returns Pango's *logical* extent (roughly
  the cursor-advance width), which for most glyphs comfortably contains the
  glyph's own visible ink once the usual `+12px` pad is added — but a direct
  Pango probe (`pango_layout_get_pixel_extents()`, which returns both ink and
  logical rects) showed this specific glyph draws ink **14px wide at 14px
  bold, while its own logical/advance width is only 8px** — a 6px gap, far
  larger than any other glyph in this project hits (the CPU icon, for
  comparison, is 11px ink vs. 8px logical — comfortably absorbed by the usual
  padding). Sized from the *logical* width with the reference's own tight
  `+8px` pad, roughly 2px of the glyph's own mantle-colored "cutout" ink fell
  outside the sapphire box, where it blended invisibly into the matching
  background — visually truncating the logo. Fixed by adding
  `module_box_measure_text_ink()` (`render/module_box.c`/`.h`) and sizing
  this one box from that instead, plus reverting to the standard `+12px` pad
  (no longer pixel-matching the reference's own tighter number, since that
  number was tuned against the wrong measurement to begin with).
  `module_box_draw_text_centered()`'s own internal centering still uses the
  *logical* width (via `module_box_measure_text`), which is fine — the wider
  box computed from the ink extent gives it enough slack on both sides
  regardless.
- **One full-screen `zwlr_layer_surface_v1` per currently-known output**
  (`LAYER_OVERLAY`, all four edges anchored, no margin, matching the reference's
  own `Variants { model: Quickshell.screens }` — each screen gets its own
  `PanelWindow` with its own centered button row, not one surface spanning a
  combined virtual desktop) — same `popup_surface_create(..., 0, 0, ...,
  ANCHOR_TOP|BOTTOM|LEFT|RIGHT, ...)` shape every other popup's *click-catcher*
  surfaces already use (see `sysmon/sysmon_info_popup.c`'s `create_catchers()`),
  except this surface actually draws visible content instead of staying
  transparent. The real per-output pixel size is read back from `Output.width`/
  `Output.height` (already tracked from `wl_output`'s own `mode` event) rather
  than from the popup surface's `configure` callback, since `popup_surface.c`
  doesn't expose that size back to its caller — safe here because a
  full-anchored, zero-margin popup surface's real size is always exactly its
  own output's size.
- **The overlay's own full-screen surface doubles as its own click-catcher** —
  the one place in this project where a popup doesn't need `tray_context_menu.c`'s
  usual separate transparent catcher-surface-underneath-a-smaller-menu shape,
  since the menu content already covers the entire output by itself.
  `power_menu_handle_click()` hit-tests the three buttons directly; landing on
  one triggers that action, landing anywhere else (the dimmed background) just
  closes the menu — both paths call `power_menu_toggle()`'s own internal
  `close_overlay()` unconditionally first, matching the reference's own
  `PowerMenuState.close()` called before dispatching whichever action fired.
- **Background dim is 30% alpha over `COLORS.crust`, painted with
  `CAIRO_OPERATOR_SOURCE`, not the default `OVER`**
  (`cairo_set_source_rgba(cr, COLORS.crust.r, ..., 0.302)`), matching the
  reference's own `"#4D" + Config.colors.crust.substring(1)` (`0x4D / 255 ≈
  0.302`) — the alpha value confirmed pixel-exact against a real screenshot
  during development (`grim` + sampling the rendered PNG's own RGB values
  landed exactly on `COLORS.yellow`/`sky`/`red`'s known hex values and each
  button's exact 150×150 + 24px-gap geometry), but the *operator* was a real
  bug caught live (the user reported the background getting darker on hover),
  not a hypothetical. This surface is backed by one of two ping-ponged shm
  buffers (`wayland/shm_buffer.c`), so whichever buffer a given redraw lands
  in still holds *its own* previous frame's content, not a blank slate — every
  other popup in this project either paints something fully opaque (which
  safely overwrites stale content under any operator) or is genuinely
  transparent (`draw_transparent()`, which already used `SOURCE` for exactly
  this reason), so this dim layer was the first *translucent* full-surface
  paint in the codebase, and the first place this went unnoticed. Painted with
  the default `OVER`, every hover-triggered `redraw_all()` call (see below)
  blended another 30%-alpha layer on top of whatever that buffer already had
  — confirmed with a standalone test replicating the exact scenario: 5
  redraws into the same un-cleared buffer climbed from alpha 77/255 to
  212/255 under `OVER`, while `SOURCE` held constant at 77/255 (≈30%) across
  all 5. `SOURCE` replaces the destination outright regardless of its prior
  contents, which is what a "redraw this popup's entire content from
  scratch" call should always do.
- **Buttons are 150×150, not the reference's own 180×180** — matches this
  project's established "everywhere else runs a bit more compact than the
  reference's Qt-driven sizing" precedent (see `modules/media/media_popup.c`'s
  own thumbnail scale-down for the same reasoning), even though a full-screen
  overlay has plenty of room to spare. `FONT.display` (a new size tier, 32px —
  the reference's own `config.js` uses 40) was added to `config.h`'s
  `FontConfig` specifically for the button icons, matching the reference's own
  dedicated `font.sizes.display` tier that (per the reference's own `CLAUDE.md`)
  "exists only for this one use."
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
  right now." Blocking happens early in `main()`, before `pulse_client_init()`
  spawns its own background thread — POSIX signal masks are inherited at
  thread-creation time, so this ensures that thread's mask blocks `SIGUSR1`
  too (the correct multi-threaded-process pattern for `signalfd`: block
  everywhere, only the thread holding the `signalfd` itself ever actually
  reads a pending instance). Add a keybind like this to `hyprland.conf` to
  wire it up:
  ```
  bind = SUPER, Escape, exec, pkill -SIGUSR1 -x stickBar
  ```
  (any keybind works — `SUPER, Escape` is just one reasonable choice; `pkill
  -x` matches the process name exactly, so it won't accidentally signal an
  unrelated process with "stickBar" as a substring of its own command line).
- **The power menu doesn't coordinate with any other popup kind** — same
  undefended, rare v1 edge case as every other popup pair's own equivalent
  limitation elsewhere in this bar (see "PulseAudio / volume & mic" above for
  the fullest explanation of why). In practice this matters more here than
  for the others, since the power menu is a full-screen modal that visually
  sits on top of everything (`LAYER_OVERLAY`, like every other popup) but
  doesn't force any other already-open popup closed first — opening the power
  menu while e.g. the tray context menu is already open leaves both
  technically open at once, though the power menu's own full-screen surface
  will visually cover the other in practice.

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
- **Tray: no fallback if stickBar's own Watcher registration itself fails**
  (e.g. `sd_bus_add_object_vtable()` erroring after the bus name was already
  claimed — logged to stderr, name released, falls back to Host mode) *and*
  no other Watcher happens to be running either — the tray then stays empty
  for the run, same underlying "nothing to connect to" failure mode as before,
  just less likely to actually occur now that stickBar provides its own
  Watcher by default.
- **Tray: no submenu/nested-menu support** — `GetLayout`'s recursion depth is
  capped at 1, matching the reference's own flat rendering.
- **Tray: no tooltip support** — the SNI `ToolTip` property is never fetched.
- **Tray: no scroll-wheel `Scroll()` support** — `wl_pointer`'s `axis_discrete`
  event is wired up project-wide now (see "PulseAudio / volume & mic" above),
  but only the volume/mic modules act on it; the tray's own `Scroll()` SNI
  method is simply never called.
- **Tray: no context-menu reflow** if it would overflow past the output's left
  edge (clamped, not repositioned) — also true of the reference.
- **Tray: no keyboard dismiss (Escape)** for the context menu — click-outside
  only, consistent with no keyboard handling existing anywhere in the project.
- **Tray: at most one context menu open at a time** — a deliberate v1
  simplification (`tray_context_menu.c`'s state is a single static singleton).
- **Volume/mic: the tray's context menu and the audio device popup are
  independent singletons that don't close each other** — opening one while
  the other is already open (e.g. right-click a tray icon, then without
  dismissing it, right-click the volume box) leaves both open simultaneously,
  an undefended, rare v1 edge case (see "PulseAudio / volume & mic" above for
  why they weren't made to coordinate).
- **Volume/mic: scroll direction wasn't independently verified** against a
  real mouse (no working synthetic-input setup in the development
  environment) — reasoned from the Wayland protocol spec text and
  conventional Linux scroll-to-adjust mapping only; see "PulseAudio / volume &
  mic" above for exactly what to flip if it's backwards in practice.
- **Volume/mic: no visual "no PulseAudio-compatible server" state** — if
  `pulse_client_init()` fails, the volume/mic boxes still render (0%, not
  muted, since every accessor returns a safe default when `g_mainloop` is
  NULL) rather than being hidden or showing an explicit error, unlike the
  tray's `count == 0` → zero-width behavior. Low-risk in practice (this
  failure mode requires a broken/absent PipeWire or PulseAudio installation
  entirely) but worth knowing if it's ever actually hit.
- **Volume/mic: device popup row count is capped at 32** and the popup's size
  is fixed at open time — a live device appearing/disappearing while the
  popup is open updates existing rows' content (icons, percent, default
  marker) live, but won't resize the popup to add/remove a row for it.
- **Mixer: playback streams only** — apps recording from a mic (PA source
  outputs) aren't listed; no per-app output-device routing either (that's
  `pa_context_move_sink_input_by_index`, not wired up). Opening and live
  row add/remove were verified via a temporary debug hook + `grim`, but the
  slider click/drag, scroll, and mute toggle were **not** exercised with a
  real pointer (no `/dev/uinput` access in the dev environment this pass) —
  the drag path is reasoned from the implicit-grab semantics.
- **CPU/GPU/memory: GPU stats are NVIDIA-only, via `nvidia-smi`** — no AMD
  (`radeontop`/`rocm-smi`) or Intel (`intel_gpu_top`) equivalent. On a
  machine with a non-NVIDIA GPU, `sysmon_gpu_available()` stays permanently
  false and the GPU module always shows `--` — not a crash, matches the
  reference `SystemMonitor.qml`'s own NVIDIA-only `Process` command.
- **CPU/GPU/memory: `nvidia-smi` runs synchronously, blocking the whole event
  loop for ~15-45ms once per 5-second poll** — a deliberate, measured
  tradeoff (see "System monitor (CPU/GPU/memory)" above for the reasoning and
  measurement), not something to "fix" reflexively by making it async without
  weighing that tradeoff again first.
- **CPU/GPU/memory: none of the three popups coordinate with the tray's
  context menu or the audio device popup** — same undefended, rare v1 edge
  case as volume/mic's own equivalent limitation above; opening a sysmon
  popup while a tray/audio popup is already open (or vice versa) can leave
  both open simultaneously.
- **CPU/GPU/memory: no left-click action, and no reflow if a popup would
  overflow the *right* edge of the screen** — the latter is the mirror image
  of the tray's own documented "no reflow past the left edge" limitation;
  no real caller today is close enough to the right edge for this to matter
  in practice (see "System monitor (CPU/GPU/memory)" above), but it's
  undefended if that ever changes.
- **CPU/GPU/memory: per-core CPU usage is capped at 128 tracked cores**
  (`MAX_CPU_CORES` in `sysmon_client.c`) — comfortably above this development
  machine's 16 threads, but a genuinely bigger machine's excess cores would
  silently be dropped from both the aggregate calculation's *display* (though
  not from `/proc/stat`'s own aggregate `cpu` line, which is still parsed and
  used for the overall percentage) and the dropdown's per-core grid.
- **Clock: no week-strip hover preview** — only the reference's year-calendar
  grid is reproduced, not `ClockWeekPopup.qml`'s hover-triggered current-week
  strip; see "Clock year-calendar popup" above for why.
- **Clock: the calendar popup doesn't coordinate with any other popup kind**
  — same undefended, rare v1 edge case as volume/mic's and CPU/GPU/memory's
  own equivalent limitations above.
- **Clock: no reflow if the popup would overflow past the output's left
  edge** (clamped, not repositioned) — same limitation as the tray's own
  context menu, inherited from reusing its identical anchor math.
- **Media: no click-to-seek on the timeline, no per-player volume/mute
  control** — both deliberately out of scope for this pass; see "Media
  (MPRIS)" above for the specific reasoning behind each.
- **Media: thumbnails only load for `file://` art URLs** — `http(s)://` art
  (Spotify's own cover-art CDN, most in-browser media) always falls back to
  the placeholder square, since this project has no HTTP client; see "Media
  (MPRIS)" above.
- **Media: playback position is only ever polled while the dropdown is open**
  (once a second, piggybacked on the existing clock timer) — the bar widget
  itself shows no position/duration at all, only the dropdown does; this
  matches the reference `MediaWidget.qml`, which doesn't show a timeline on
  the bar widget either (only `MediaPopup.qml` does).
- **Media: `PlayPause` is sent unconditionally**, not gated on the player's
  own `CanPlay`/`CanPause` capabilities the way the reference's
  `togglePlaying()` is — see "Media (MPRIS)" above for why that gating wasn't
  replicated. In practice this only matters for a player that both lacks
  `PlayPause` *and* reports `CanPlay`/`CanPause` as false, which would be an
  unusual/broken MPRIS implementation.
- **Media: the popup doesn't coordinate with any other popup kind** — same
  undefended, rare v1 edge case as volume/mic's, CPU/GPU/memory's, and the
  clock's own equivalent limitations above.
- **Media: player list is capped at 16 tracked players**
  (`MAX_MPRIS_PLAYERS` in `mpris_client.c`) — comfortably above any real
  desktop's simultaneous player count, but a genuinely pathological case
  would silently drop players past that cap, same "documented cap, not
  dynamic growth" convention as every other fixed-array table in this
  project (`sni_registry.c`'s `MAX_TRAY_ITEMS`, `device_popup.c`'s
  `MAX_DEVICE_ROWS`).
- **Network: no VPN/tunnel-interface awareness** — this module (like the
  reference) only ever looks at whichever interface currently owns the
  default route; a VPN that adds its own default route makes this correctly
  show the VPN's own interface/IP, but there's no indication *that* it's a
  VPN specifically (no icon variant, no "via" annotation).
- **Network: no signal-strength/quality display for Wi-Fi** — `/proc/net/
  wireless` is read only to answer "is this interface Wi-Fi at all," not
  parsed for its quality/link-level columns, matching the reference's own
  `NetworkMonitor.qml`, which reads the same file for the same yes/no
  purpose only.
- **Network: not independently verified against a real Wi-Fi adapter** — see
  "Network / Wi-Fi" above for exactly which code paths that affects and what
  to re-check first if a bug ever surfaces specifically on Wi-Fi.
- **Network: DNS server list is capped at 8 entries**
  (`MAX_DNS_SERVERS` in `network_client.c`) — comfortably above the 3 glibc's
  own resolver actually honors from `/etc/resolv.conf`, so this is a
  theoretical cap rather than a real limitation in practice.
- **Network: the popup doesn't coordinate with any other popup kind** — same
  undefended, rare v1 edge case as every other module's own equivalent
  limitation above.
- **Privacy: webcam vs. screen-share classification not independently
  verified** — this development machine has no `/dev/video*` device and
  driving `xdg-desktop-portal-hyprland`'s screen-share picker needs
  interactive UI a headless test can't provide, so `video_node_is_webcam()`'s
  elimination heuristic (see "Privacy indicator" above) was never exercised
  against real hardware/a real portal session — only mic detection was. If
  webcam/screen-share ever shows the wrong icon in practice, that function is
  the first thing to re-check.
- **Privacy: no per-app mute/stop control** — the dropdown is read-only, same
  "list what's active" scope as `sysmon_info_popup.c`'s CPU/GPU/memory
  dropdowns; there's no equivalent of PipeWire's own ability to suspend a
  node from this bar.
- **Privacy: node/link tables are capped at 256 entries each**
  (`MAX_PW_NODES`/`MAX_PW_LINKS` in `pipewire_client.c`) — comfortably above
  a typical desktop's PipeWire graph size, same "documented cap, not dynamic
  growth" convention as every other fixed-array table in this project.
- **Privacy: the popup doesn't coordinate with any other popup kind** — same
  undefended, rare v1 edge case as every other module's own equivalent
  limitation above.
- **Power menu: no keyboard handling at all — Escape doesn't close it**, unlike
  the reference's own `Keys.onEscapePressed`. This project has no keyboard
  input plumbing anywhere (no `wl_keyboard` binding, no libxkbcommon
  dependency), and adding one purely to support Escape-to-close here — when
  click-outside-to-close (the same dismiss mechanism every other popup in
  this bar already uses) already covers the primary use case — wasn't judged
  worth the new subsystem for v1. Click the dimmed background, or either
  trigger again (the button, or the SIGUSR1 keybind), to dismiss instead.
- **Power menu: the two subprocess-launching actions (Restart/Shutdown) have
  no confirmation step** — a single click immediately runs `systemctl
  reboot`/`systemctl poweroff`, matching the reference exactly (it has no
  confirmation dialog either), so this is parity, not a regression, but
  worth knowing if a stray click is a real concern on a given setup.
- **Power menu: doesn't coordinate with any other popup kind** — see "Power
  menu" above for why this one's version of the usual undefended-popup-
  coordination limitation matters slightly more than the others (the overlay
  is full-screen and visually covers whatever else was open, without
  actually closing it).

## Scope / roadmap

Implemented: bar shell (per-output layer-surface, double-buffered shm rendering),
workspace indicator (kanji pills, occupied/focused logic, click-to-focus), clock
(with a right-click year-calendar dropdown — see "Clock year-calendar popup"),
system tray (SNI item tracking, icon rendering, DBusMenu context menu, click routing),
volume/mic (default sink/source display, click-to-mute, scroll-to-adjust, device
dropdown for picking the default sink/source — see "PulseAudio / volume & mic"),
CPU/GPU/memory monitoring (three independent left-region modules, each with its own
accent color and right-click info dropdown, split from the reference's single
combined widget per explicit user request — see "System monitor (CPU/GPU/memory)"),
a media (MPRIS) player widget (centered in the bar, click-to-play-pause, a
right-click dropdown listing every tracked player with prev/play-pause/next
controls — see "Media (MPRIS)"), a network/Wi-Fi status widget (default-route
interface display with a live rx/tx rate, a right-click dropdown with
interface/IP/gateway/MAC/DNS details — see "Network / Wi-Fi"), a power-menu
overlay (an Arch-logo bar button plus a `SIGUSR1`-driven Hyprland keybind, both
toggling a full-screen Logout/Restart/Shutdown overlay — see "Power menu"), and
a privacy indicator with no reference-bar equivalent (mic/webcam/screen-share
usage via a direct PipeWire stream-graph monitor, collapsing to zero width when
nothing's active, a right-click dropdown naming which app is using which — see
"Privacy indicator (mic/webcam/screen-share)").

Not yet implemented: nothing from the original reference-bar feature set remains
unported. Future additions should follow the existing module shape: a
`modules/<name>/` directory, a render function taking
`(cairo_t*, ..., x, box_top, box_height)` and returning the next x coordinate, and
— if it needs live data — either a Hyprland IPC event hook (if relevant) or its
own poll/event source integrated into `main.c`'s epoll loop the same way the
clock's timerfd, Hyprland's event socket, the tray's/media module's shared D-Bus
connection, the volume/mic modules' PulseAudio connection, the CPU/GPU/memory
modules' own 5-second timerfd, and the power menu's own SIGUSR1 signalfd are.
