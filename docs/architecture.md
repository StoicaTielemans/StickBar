# Architecture

## Rust layout (ported so far)

The Rust port keeps the C bar's shape: one directory per concern (`mod.rs` inside),
the same names. Everything below this section still describes the **C bar**, the
reference for the parts not yet ported (progress: [`porting.md`](porting.md)).

```
src/
├── main.rs                 entry point, the C main.c: signals first, connect, bind,
│                           the calloop event loop, exit status (0 on SIGINT/SIGTERM,
│                           1 when the compositor connection is lost)
├── wayland/
│   ├── mod.rs              App: all state (the AppContext equivalent), the SCTK
│   │                       handler impls (frame callbacks, layer configure/closed,
│   │                       shm, registry), and the delegate macros wiring them up
│   ├── pointer.rs          seat + pointer; App::pointer_button/pointer_motion, the
│   │                       one routing path real and injected input share
│   ├── shm_buffers.rs      ShmBuffers: a surface's two shm buffers (bars + popups)
│   ├── viewporter.rs       optional wp_viewporter bind (SCTK has no helper)
│   └── output.rs           Output: one tracked monitor (index, name, its bar);
│                           OutputHandler: a record + bar per monitor, the
│                           STICKBAR_TEST_OUTPUT filter
├── bar/
│   ├── bar_surface.rs      BarSurface: the layer surface, two shm buffers, the
│   │                       prev/next cairo frames, Dirty (clean/clock/full) +
│   │                       frame-callback pacing, draw -> diff -> commit only the
│   │                       changed rectangle, and the boxes of the last frame
│   ├── damage.rs           changed_rect(): the frame diff, with unit tests
│   ├── popup_surface.rs    PopupSurface: an overlay menu or a top-layer catcher
│   │                       (1x1 + viewporter), first-paint-after-configure
│   ├── popup_host.rs       PopupHost: a menu under its box + a catcher per monitor
│   └── popups.rs           Popups: every dropdown kind; configure/menu routing
├── hyprland/
│   ├── mod.rs              socket_path(), request()/request_json(): the request socket
│   ├── events.rs           the event socket as a calloop source; line framing, filter
│   └── dispatch.rs         Dispatcher: Lua form first, classic fallback, cached
├── sysmon/
│   ├── mod.rs              Sysmon: CPU/GPU/memory figures + histories, poll()
│   ├── cpu.rs              /proc/stat deltas, temperature, P/E split, process sample
│   ├── gpu.rs              NVML (nvml-wrapper): usage/temp per poll, details on open
│   ├── memory.rs           /proc/meminfo, PSI, zram, top apps by private memory
│   └── timer.rs            the 5 s poll (calloop Timer), shared with the network
├── network/mod.rs          Network: default route, rates, history; details via
│                           /sys, resolv.conf and getifaddrs (nix)
├── util/history.rs         History2: the 120-sample ring behind every graph
├── modules/
│   ├── cpu/                mod.rs (box), popup.rs (the CPU dropdown)
│   ├── gpu/                mod.rs (box), popup.rs (the GPU dropdown)
│   ├── mem/                mod.rs (box), popup.rs (the memory dropdown)
│   ├── network/            mod.rs (box), popup.rs (the network dropdown)
│   ├── workspaces/mod.rs   Workspaces: occupied/focused from Hyprland, kanji pills
│   ├── powermenu/
│   │   ├── mod.rs          PowerMenu: full-screen overlay per monitor, cards, the
│   │   │                   record-only test guard (actions run in wayland/pointer.rs)
│   │   └── arch_logo.rs    the sapphire Arch block (sized from the glyph's ink)
│   └── clock/
│       ├── mod.rs          Clock: cached text, tick(), its box
│       ├── calendar.rs     CalendarPopup: the year calendar (chrono date maths)
│       └── timer.rs        wall-clock-aligned timerfd (rustix), ECANCELED re-arm
├── render/
│   ├── mod.rs              Renderer (shared by every bar: text cache + modules),
│   │                       render_output(), render_clock_only(), BoxRect/ModuleBox,
│   │                       ClickTarget/Action (clickable spots inside a box)
│   ├── text.rs             TextCache: pango context, font descriptions, LRU of 64
│   │                       shaped layouts; all text goes through here
│   ├── module_box.rs       draw_rect() and label(): the bordered module box
│   ├── braille_graph.rs    btop-style dot graphs (pure cairo)
│   ├── meter_bar.rs        segmented meters
│   ├── stat_card.rs        graph card sizing, axis, label/value and top-apps rows
│   ├── popup_chrome.rs     draw_frame(), draw_card(), draw_header(), draw_pill()
│   └── popup_style.rs      shared dropdown layout tokens (margins, header, pill)
├── testing/mod.rs          the test-control channel on stdin (STICKBAR_TEST_CONTROL=1)
└── config/
    ├── colors.rs           Color + the Catppuccin Mocha palette
    ├── bar.rs              BAR geometry
    ├── fonts.rs            FONT: family and sizes
    └── glyphs.rs           Nerd Font icons as '\u{...}' char consts
```

How an event becomes pixels: SCTK reads the Wayland socket and calls a handler
method on `App` (`&mut App`, so all state is in that one struct). Handlers only
mark bars dirty; after each loop pass, `main.rs` calls `App::flush_dirty()`, which
draws each dirty bar that isn't waiting on a frame callback, so a burst of
events in one pass is drawn once.

# C bar architecture (reference)

## File layout

One subdirectory per concern, and one directory per bar widget under `modules/` —
this is a deliberate, explicit choice (see "Conventions" in [`CLAUDE.md`](../CLAUDE.md)), not the minimum
viable structure for a project this size.

```
stickBar/
├── meson.build
├── protocols/
│   └── wlr-layer-shell-unstable-v1.xml   # vendored
├── docs/                         topic documentation, indexed from CLAUDE.md
├── tests/                        live regression tests + perf_compare.py (see
│                                 docs/testing.md and tests/README.md)
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
    │   │                         wayland/* and bar/* file (see "Core data model" below)
    │   ├── registry.c            wl_registry global binding + the connect/roundtrip
    │   │                         sequence
    │   ├── event_loop.c          epoll setup + the wl_display prepare_read dance
    │   ├── output.c              wl_output tracking, geometry/mode/scale/done/name/
    │   │                         description handling, per-output index assignment
    │   ├── pointer.c             wl_seat -> wl_pointer, enter/motion/button routing
    │   └── shm_buffer.c          memfd-backed double-buffered wl_shm pool
    ├── util/                     generic helpers several subsystems share: timerfd
    │                             create/arm/drain (timer.c), eventfd notification
    │                             (notify_fd.c), /proc and /sys reads (file.c), the
    │                             two-series history ring behind every stat graph
    │                             (history.h)
    ├── testing/
    │   └── test_control.c        STICKBAR_TEST_CONTROL=1 stdin command channel
    │                             for tests/ (injected clicks, data dumps, popup
    │                             PNG snapshots); inert otherwise
    ├── bar/
    │   ├── bar_surface.c         per-output zwlr_layer_surface_v1 lifecycle +
    │   │                         frame-callback-throttled redraw (lives forever,
    │   │                         one per output)
    │   ├── popup_surface.c       generic *transient* layer-surface: explicit
    │   │                         anchor/margin/size, single-shot render (no
    │   │                         frame-callback throttling), created per-open and
    │   │                         destroyed on close -- every dropdown, click-catcher
    │   │                         and the power menu (see docs/popups.md)
    │   ├── popup_host.c          what every dropdown shares around its content:
    │   │                         the surface placed under its box, the click-
    │   │                         catchers, open/close/resize, surface matching
    │   └── strip_surface.c       small desync wl_subsurface drawn on its own --
    │                             the media dropdown's spectrum graphs (the only
    │                             continuous animation)
    ├── render/
    │   ├── render.c              top-level per-output draw dispatch + background
    │   ├── module_box.c          shared cairo/pango helpers: bordered box chrome,
    │   │                         text measure/draw (left/centered/right) through
    │   │                         a cache of shaped Pango layouts, ellipsis
    │   │                         truncation -- all text goes through here
    │   ├── color.h               Color helpers: set source, alpha, 3-stop gradient
    │   ├── box_rects.c           where each bar box was last drawn, per output: the
    │   │                         click hit-test and dropdown anchor every module uses
    │   ├── braille_graph.c       btop-style braille-dot history graph (pure
    │   │                         cairo), used by the network/CPU/GPU/memory
    │   │                         dropdowns; plus a lit-sprite + column-rect path
    │                             for the animated media spectrum
    │   ├── meter_bar.c           segmented "how full" bar (btop's ▰▰▱▱ meters),
    │   │                         pure cairo, used by the CPU/GPU/memory dropdowns
    │   ├── popup_style.h         layout tokens every dropdown shares (margins, header,
    │   │                         pill, gaps)
    │   ├── popup_chrome.c        dropdown chrome: frame, header row + pill, cards
    │   └── stat_card.c           the CPU/GPU/memory/network dropdowns' shared parts:
    │                             graph card sizing, history window, time axis, label/
    │                             value rows, top-apps rows
    ├── hyprland/
    │   ├── ipc_request.c         request socket (.socket.sock): one connection per
    │                             command, cJSON parsing helper
    │   ├── ipc_events.c          event socket (.socket2.sock): persistent
    │   │                         connection, line framing, event->refresh trigger
    │   └── dispatch.c            click-to-focus dispatch with the classic->Lua
    │                             fallback quirk (see docs/modules/hyprland.md)
    ├── dbus/                     D-Bus plumbing for the system tray (see
    │                             docs/modules/tray.md)
    │   ├── dbus_loop.c           sd_bus connection + generic epoll integration
    │   ├── dbus_dict.c           a{sv} property-dictionary walker shared by MPRIS,
    │   │                         SNI and DBusMenu parsing
    │   ├── sni_watcher_service.c attempts to *become* the StatusNotifierWatcher;
    │   │                         tried first
    │   ├── sni_watcher.c         StatusNotifierWatcher Host (client) registration;
    │   │                         fallback used only if _service.c's claim fails
    │   ├── sni_registry.c        tracked-item table, property fetch, per-item
    │   │                         signal subscriptions
    │   ├── sni_icon.c            pure IconPixmap -> cairo_surface_t* conversion
    │   │                         (no D-Bus types in this file, deliberately)
    │   ├── icon_theme.c          fallback PNG icon lookup for items with no usable
    │   │                         IconPixmap -- also no D-Bus types
    │   └── dbusmenu.c            com.canonical.dbusmenu client: GetLayout parsing,
    │                             Event("clicked",...) triggering
    ├── audio/                    PulseAudio-protocol plumbing for volume/mic (see
    │                             docs/modules/audio.md)
    │   ├── pulse_client.c        pa_threaded_mainloop connection, live sink/source/
    │   │                         stream cache, get+set, eventfd notification
    │   ├── device_popup.c        shared sink/source dropdown (volume and mic)
    │   ├── mixer_popup.c         per-app volume mixer (middle-click on the volume box)
    │   └── volume_style.c        how volume looks: level icon, meter gradient, slider
    │                             percent -- shared by the volume box and both popups
    ├── sysmon/                   /proc + NVML plumbing for CPU/GPU/memory (see
    │                             docs/modules/sysmon.md)
    │   └── sysmon_client.c       the 5s poll, plain synchronous accessors, and the
    │                             dropdown-only details (process scans, PSI, zram,
    │                             clocks)
    ├── mpris/                    MPRIS plumbing for the media module (see
    │                             docs/modules/media.md), on dbus/'s shared session-bus connection
    │   └── mpris_client.c        tracked-player table, subscriptions, fire-and-forget
    │                             controls, on-demand Position, duplicate hiding
    ├── network/                  plumbing for the network module (see
    │                             docs/modules/network.md), on sysmon's shared 5s timerfd
    │   └── network_client.c      /proc, /sys, resolv.conf and getifaddrs() reads
    ├── pipewire/                 stream-graph monitor for the privacy module (see
    │                             docs/modules/privacy.md)
    │   └── pipewire_client.c     pw_thread_loop, node/link/client tables, eventfd
    │                             notification, player <-> playback stream matching
    ├── visualizer/               per-player stereo spectrum graphs in the media
    │                             dropdown (see docs/modules/media.md); runs only
    │                             while it's open
    │   ├── visualizer.c          up to 4 pw_stream capture slots (a player's own
    │   │                         stream, or the output mix), 15fps timerfd,
    │   │                         smoothed levels + peaks per channel
    │   └── spectrum.c            pure FFT + log-spaced bands, no PipeWire types
    ├── eq/                       DAC EQ plumbing for the EQ module (see
    │                             docs/modules/eq.md), on its own inotify fd
    │   ├── eq_state.c            state.json load + inotify watch + background
    │   │                         `blackpearl-eq.py profile NAME` launcher
    │   └── eq_curve.c            pure cairo curve drawing, shared by the bar box and
    │                             dropdown
    └── modules/                  each box's render + hit-rect + click handling; which
                                  button does what is in docs/popups.md
        ├── workspaces/
        │   └── workspaces.c      occupied/focused set computation, kanji pills
        ├── cpu/                  cpu.c (box), cpu_popup.c (dropdown)
        ├── gpu/                  gpu.c (box, NVIDIA only), gpu_popup.c (dropdown)
        ├── mem/                  mem.c (box), mem_popup.c (dropdown)
        ├── privacy/              privacy.c (box), privacy_info_popup.c (dropdown)
        ├── clock/                clock.c (timerfd-driven box),
        │                         clock_calendar_popup.c (year calendar)
        ├── volume/               volume.c (default-sink box)
        ├── mic/                  mic.c (default-source box)
        ├── media/                media.c (active-player box, centered in the bar),
        │                         media_popup.c (dropdown)
        ├── network/              network.c (default-route box), network_info_popup.c
        ├── eq/                   eq.c (mini-curve + profile box, between volume and
        │                         network), eq_popup.c (dropdown)
        ├── tray/                 tray.c (icon row, Activate vs open-menu routing),
        │                         tray_context_menu.c (DBusMenu dropdown)
        └── powermenu/            arch_logo.c (leftmost bar item), power_menu.c (the
                                  overlay, see docs/modules/power-menu.md)
```

## Core data model

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
  `bar/*`, `render/*`, or `modules/*`; `bar/bar_surface.c` depends on
  `render/render.h`; `render/render.c` depends on `modules/*`. `wayland/pointer.c`
  is the one exception: it calls directly into each module's and popup's
  click/motion/scroll handlers -- a flat list of explicit call sites, not a generic
  dispatch table (at N=11 clickable bar-surface modules plus 7 popup kinds, still
  cheap enough that the indirection wouldn't pay for itself).
- **`AppContext.pointer_over` vs `pointer_over_surface`**: `pointer_over_surface`
  is the raw `wl_surface*` currently under the pointer, always kept up to date.
  `pointer_over` is the `Output*` it resolves to *if* it's one of the bar's own
  surfaces — `NULL` for any popup or click-catcher surface, since those are
  never stored on any `Output` (only `bar_surface.c`-owned surfaces are).
  `pointer.c`'s button handler checks each popup kind's surfaces in turn, each via
  its own `_is_menu_surface()` / `_is_catcher_surface()` pair (they're disjoint, so
  the order among them doesn't matter), *then* falls back to the `Output`-keyed
  path -- a popup/catcher surface never matches an `Output`, so checking the popup
  kinds first is required, not just tidier.

## Redraw path (`bar/bar_surface.c`)

Every `output_mark_all_dirty()` call still means "something may have changed,"
but a frame only reaches the compositor if it actually differs:

- **Marking only sets flags; `main.c` draws once per event-loop iteration**
  (`output_flush_dirty()`, right before waiting for events). So a wakeup that
  delivers several changes -- a burst of MPRIS or tray signals, each marking the
  bar dirty -- costs one render, not one per signal.
- **Most event sources mark the bar dirty only when what it shows changed**:
  pulse (default sink/source volume and mute), PipeWire (the privacy flags),
  Hyprland (occupied/focused workspaces), MPRIS (a player's shown state). The
  pixel comparison below would drop an unchanged frame anyway, but only after
  laying out and drawing it.

- **Two private CPU-side frames per bar surface (`prev`/`next`)**, besides the
  two shm buffers. `prev` is exactly what was last committed; the shm buffers
  can't serve that role because they ping-pong with the compositor, so the
  one you get back holds a frame from *two* commits ago.
- **Skip if identical**: each redraw draws into `next` and compares it to
  `prev`. No difference → no attach/commit/frame callback at all. This is
  the one check that covers every redraw source (Hyprland events that don't
  change a pill, PipeWire graph churn, 5s polls landing on the same numbers,
  `pactl` setting a volume to its current value — the last one confirmed: 40
  such calls produced zero extra commits).
- **Damage is the bounding box of changed pixels**, not the whole surface —
  a clock tick damages ~8x10 px (the last minute digit), a volume change ~32x11.
  Confirmed via `WAYLAND_DEBUG=1` (`grep damage_buffer`).
- **The clock shows no seconds, so it repaints once a minute**: the timer still
  fires every second (the media and CPU dropdowns ride on it), but
  `clock_tick()` returns whether the text changed and `main.c` only marks the
  clock dirty then. Measured with `tests/perf_compare.py` (90s, vs. the
  seconds build): commits 143 -> 61/min, idle CPU 0.19% -> 0.10%; what's left is
  mostly the 5s polls on both outputs. `STICKBAR_TEST_CLOCK_SECONDS` brings the
  seconds back, for the two tests that need a per-second change.
- **A clock tick is clock-only**: `output_mark_all_clock_dirty()` →
  `render_output_clock_only()` copies `prev` into `next` and repaints only the
  clock's box (clipped, at its last recorded rect from `clock_last_rect()`).
  It falls back to a full `render_output()` if a full redraw is also pending,
  there's no previous frame, or the clock box came out a different width (the
  boxes to its left would need to move). Any full redraw request wins over a
  clock-only one.
- **Continuous animation goes on a subsurface, not through this path**: the
  bars used to be on the bar too, and repainting them inside it (copy `prev` ->
  draw -> diff -> commit, like the clock) cost about twice what a separate
  `bar/strip_surface.c` subsurface does. The spectrum now lives only in the media
  dropdown -- see [modules/media.md](modules/media.md).
- **The clock timer is aligned to wall-clock seconds** (`arm_clock_timer()` in
  `main.c`): a `CLOCK_REALTIME` timerfd armed with `TFD_TIMER_ABSTIME` for the
  next whole second, 1s interval. `TFD_TIMER_CANCEL_ON_SET` makes a
  wall-clock *set* (NTP step, manual change, resume from suspend) fail the
  next `read()` with `ECANCELED`, and the loop re-arms to re-align. The `ECANCELED` re-arm path wasn't
  exercised live (it needs root to set the clock).

## Performance (measured)

Measured on this machine (2x 1920x1080 outputs) with fresh instances of the
build before and after NVML + the redraw path above, run side by side:

| | before | after |
|---|---|---|
| CPU, incl. spawned children | ~1.2% | ~0.3% |
| `nvidia-smi` processes per minute | 12 | 0 |
| Whole 5s sysmon poll (CPU+mem+GPU) | 15-45ms+ (`nvidia-smi`) | ~0.7-1.5ms |
| Private memory (`Private_Dirty`) | ~6 MB | ~26 MB |
| RSS at startup | ~29 MB | ~55 MB |
| Commits/min (both outputs) | ~160 | ~155 |
| Pixels damaged/min | ~6.2M | ~0.5M |

- **GPU**: the bar itself does no GPU work (cairo draws on the CPU into
  `wl_shm`; Hyprland composites), and NVML only reads stats -- `stickBar`
  never appears in `nvidia-smi`'s process list and holds no VRAM.
- **Almost all of the memory increase is NVML** (see [modules/sysmon.md](modules/sysmon.md));
  the redraw path's `prev`/`next` frames add ~0.6 MB for two outputs.
- Commits barely dropped here because the clock still showed seconds then (it
  no longer does, see above); the win was that each commit damages a few dozen
  pixels instead of the whole bar.
- **Code cleanup pass** (release build vs. the visualizer commit `cfaf902`, same
  run, `tests/perf_compare.py`): idle CPU 0.14% -> 0.08%, memory and commits
  unchanged. Most of it is the Pango layout cache in `render/module_box.c` (every
  text measure/draw used to create and shape a fresh layout, ~30-45 per output per
  full redraw for ~13 distinct strings). Under event bursts, from the change-gated
  redraws above: 300 no-op pulse volume events 660ms -> 30ms of CPU, 20 PipeWire
  graph changes 130ms -> 10ms.
- How these were measured, to redo it: CPU from `/proc/<pid>/stat`
  utime+stime **plus cutime+cstime** (the old build's `nvidia-smi` CPU only
  shows up in the latter -- leaving it out made the old build look ~3x
  cheaper than it was); memory from `/proc/<pid>/smaps_rollup`; commits and
  damage by counting `wl_surface#N.commit` / `damage_buffer(...)` lines under
  `WAYLAND_DEBUG=1`. RSS of a long-running instance drifts down over hours as
  untouched pages get reclaimed, so compare fresh starts against fresh starts.
