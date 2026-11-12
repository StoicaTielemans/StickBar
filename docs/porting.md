# Porting the bar to Rust

## Purpose and how to use it
- This is the order the bar is ported in, and the record of how far it got. Tick a milestone
  when its definition of done holds. Update the doc in the same commit as the work.
- The C bar on `main` (folder `../StickBar`) is the reference. Read the C files listed for a
  milestone, plus that module's `docs/modules/*.md` and its `BUG.md` section, **before**
  writing the Rust version. Each milestone ports behaviour, bug-fix lessons included,
  not line-by-line C.
- One milestone ≈ one or a few sessions. Never leave the branch failing `./build.sh`.

## Definition of done (every milestone)
1. `./build.sh` passes: fmt, clippy with the full lint policy, debug **and** release.
2. The milestone's Python tests pass against `target/debug/stickBar`. The tests are
   unchanged, except for C-specific wording in failure messages.
3. Any test-control commands the tests need (`rects`, `click`, `menu`, `dump …`,
   `target …`, `snapshot`) work, and their JSON replies match the C bar's.
4. `python3 tests/perf_compare.py --against main` shows no regression in CPU, RSS or commits
   versus the C bar, and the table is pasted into the commit message (performance rule).
5. The module's `docs/` file is updated to describe the Rust code (file paths, types), and
   the `BUG.md` lessons it relies on are kept as rules.
6. The bar was run by hand for a minute on the real desktop with stderr watched.

## Architecture decisions (made once, here)
| Concern | C bar | Rust | Why |
|---|---|---|---|
| Event loop | hand-rolled epoll in `main.c` | **calloop**: one loop, every fd a source | It's what smithay-client-toolkit is built on, and it's safe |
| Wayland | raw libwayland + vendored XML | **smithay-client-toolkit** (layer-shell, shm, seat, output) + `wayland-protocols` (viewporter, subsurface), on the **system libwayland** (`wayland-backend` `client_system`) | Safe and maintained. Pin one SCTK version, since its API changes between releases. The system backend keeps `WAYLAND_DEBUG` in libwayland's wall-clock format, which the tests parse; the pure-Rust transport prints a different one |
| Drawing | cairo + pangocairo | **cairo-rs** (`png` feature), **pango**, **pangocairo** crates | The same C libraries behind safe wrappers |
| Draw → shm | cairo draws straight into the memfd | cairo draws into its own `ImageSurface`, then the bytes are copied into SCTK's `SlotPool` buffer | Drawing straight into the shm buffer needs `unsafe`. A copy is about 200 KB per bar frame. Measure it in M1 |
| Threads → main loop | eventfd (`util/notify_fd.c`) | **calloop `channel`**: worker threads send messages, the main loop wakes | Same pattern without raw fds |
| JSON | cJSON | **serde_json** | Hyprland IPC and the test-control replies |
| Hyprland IPC | own socket code | own code over `std::os::unix::net::UnixStream` | Small, and it keeps the classic→Lua dispatch quirk under our control. No async crate |
| SIGUSR1 | sigprocmask + signalfd | **calloop `signals`** source, created **first in `main`, before any thread starts** | Lesson from BUG.md "Power menu": NVML threads that didn't block SIGUSR1 killed the bar |
| D-Bus (tray, MPRIS) | sd-bus | **zbus 5** (async, on its own `dbus` thread → channels) | Pure Rust, no unsafe. zbus 6 dropped the blocking API; one async loop on one thread is simpler than several blocking iterators anyway |
| PulseAudio | libpulse threaded mainloop | **libpulse-binding** on its own thread → channel | Same threading model as the C bar |
| PipeWire | pw_thread_loop | **pipewire** crate (pipewire-rs) on its own thread → channel | Same |
| NVML | dlopen'd libnvidia-ml | **nvml-wrapper** | Loads the library at runtime, like now. Use the v2 memory call (BUG.md lesson) |
| Timers / clock | timerfd + localtime | calloop `Timer` + **chrono** (`Local`) | |
| inotify (EQ) | raw inotify | **rustix** `fs::inotify` as a calloop `Generic` source | rustix is already a dependency; its inotify API is safe, so no extra crate |
| getifaddrs (network) | libc | **nix** (`net` feature) | Safe wrapper |
| FFT (visualizer) | own `spectrum.c` | **rustfft** | |
| Child processes | fork/execv + mask reset | **nix `posix_spawn`** with an empty signal mask (the EQ script) | `std::process::Command` does **not** reset the mask (only SIGPIPE's handler), so its child keeps SIGINT/SIGTERM/SIGUSR1 blocked; `pre_exec` would need `unsafe`. `spawn_systemctl` still uses `Command` (BUG.md, Open) |

Add each crate only in the milestone that first needs it.

## Milestones
Each lists: the C files to read on `main` (rough line count), what to build, and which tests
must pass.

- [x] **M0: Skeleton.** Cargo project, lint policy, `build.sh`, tests pointed at `target/`.
- [x] **M1: An empty bar on every monitor** (`main.c`, `wayland/{registry,output,event_loop,shm_buffer}.c`,
  `bar/bar_surface.c`, `config/config.c`, `render/render.c`). Signals first, connect,
  one 20 px top layer surface per output with an exclusive zone, two shm buffers,
  frame-callback-throttled redraw with a dirty flag, draw → diff → commit only the changed
  rectangle, the Catppuccin palette, and exit 1 when the compositor goes away. The cairo toy
  text API is in `clippy.toml`'s `disallowed-methods`. Tests: `test_wayland_disconnect`,
  `test_stdin_intact`.
  *Moved out of M1:* `dead_code` is an error, so code can't be ported before something uses
  it. The pango text helpers + layout cache (`render/module_box.c`), glyphs, fonts and the
  rest of `BAR` move to M3. The output `index` moves to M5. `util/*` comes piece by piece with
  its users.
- [x] **M2: Test-control channel** (`testing/test_control.c`, the test bits of
  `wayland/output.c` and `bar/bar_surface.c`). The `STICKBAR_TEST_CONTROL` stdin channel
  (`ping`, `outputs`, `rects`; one `match` arm per command, added as modules land),
  `STICKBAR_TEST_OUTPUT`, empty input regions in test mode, per-monitor `Output` records
  with their `index` (moved here from M5, since `outputs` reports it), and the setup round trip
  so monitors are known before the first command. New guard test:
  `test_control_channel`, which passes against both bars.
  *Moved out of M2 (unused code fails the build):* box-rect recording goes to M3, the first
  box drawn. The pointer (`wl_seat`), click routing and `click`/`menu-*`/`catcher-click` go
  to M4, where the clock's right-click opening the calendar is the first thing that reacts
  to a click.
- [x] **M3: Clock** (`modules/clock/clock.c`, the text and box parts of `render/module_box.c`,
  `config/glyphs.c`, the clock bits of `render/render.c`, `bar/bar_surface.c` and `main.c`).
  The clock box, a wall-clock-aligned `timerfd` (rustix) with `ECANCELED` re-arm, the clock-only
  redraw shortcut, the pango layout cache, glyph consts verified with fontTools, box rects per
  bar (so `rects` reports the clock), and `STICKBAR_TEST_CLOCK_SECONDS`. Tests:
  `test_clock_alignment`, `test_redraw` (both parts: no pulse events reach the bar yet, so the
  no-op check passes trivially until M9). The clock box is **pixel-identical** to the C bar's
  (grim of both, byte-equal PNGs). *Deferred:* `BAR.item_spacing` (no gap with a single box)
  and the other font sizes arrive with their first users.
- [x] **M4: Pointer, popup infrastructure + year calendar** (`wayland/pointer.c`,
  `bar/popup_surface.c`, `bar/popup_host.c`, `render/popup_chrome.c`,
  `modules/clock/clock_calendar_popup.c`, the click/menu parts of `testing/test_control.c`).
  The `wl_seat` pointer, with presses and motion routed through one path (`App::pointer_button`
  / `pointer_motion`) that the channel's `click`/`menu-click`/`menu-motion`/`catcher-click` also
  use. Popup surfaces (overlay menu, top-layer catchers with a 1×1 buffer stretched by
  `wp_viewporter`), `PopupHost`, the `Popups` routing table, the frame/card chrome, `menu` and
  `snapshot`, and the calendar: chrono date maths, year navigation, hover. The shm buffer pair is
  now shared by bars and popups (`wayland/shm_buffers.rs`). The calendar is pixel-identical to
  the C bar's (opened, next year, hovered).
  `test_popups` waits for the cpu/mem/volume boxes (M7, M9), so the new **`test_calendar`** guard
  covers the clock's part plus navigation and hover. It passes against both bars, and fails
  with the first-paint fix removed. *Moved on:* left-anchored opening → M7 (the first left-region
  dropdown); host resize, button release and scroll → M9 (sliders, the mixer).
- [x] **M5: Hyprland + workspaces** (`hyprland/*`, `modules/workspaces/`, the kanji in
  `config/glyphs.c`, the left region of `render/render.c`, the reconnect policy in `main.c`).
  The request socket (blocking, 1 s timeout, `shutdown(Write)` before reading), the event socket
  (calloop source, line framing, the 12-event filter, a re-query only on a relevant read), the
  dispatcher (unquoted Lua form first, classic fallback, cached, nothing cached without a reply),
  one reconnect then give up, and kanji pills per 5-wide range. Also a generic **`ClickTarget`**:
  clickable spots inside a box (the pills now, tray icons in M11), which also get the hand
  cursor. New guard test **`test_workspaces`** runs the bar against a fake Hyprland
  (`tests/lib/fake_hyprland.py`), so a click never switches the real workspace. It passes against
  both bars. Both bars give DP-2 index 0, and the pills are pixel-identical to C's on the real
  desktop.
- [x] **M6: Arch logo + power menu + SIGUSR1** (`modules/powermenu/*`, the header/pill
  parts of `render/popup_chrome.c`, `popup_style.h`, ink width and truncation from
  `module_box.c`). The Arch logo (sized from the glyph's ink), the power menu as a full-screen
  overlay per monitor (a new `PopupSurface::overlay` kind, centered on the configured size),
  the header and pill chrome, Logout through `Dispatcher::exit`, and Restart/Shutdown through
  `systemctl`, spawned and reaped by a named thread (never blocking). SIGUSR1 toggles it. The
  test guard is kept: in test mode a card only records its action. **The C bar's own
  `test_power_menu` passes unchanged** (SIGUSR1 blocked in every thread, keybind open/close,
  centering, hover colours, the record-only guard). The Arch logo and the overlay (plain and
  hovered) are pixel-identical to C.
- [x] **M7: CPU / memory / GPU**, split into three steps (about 3,200 C lines in total):
  - [x] **M7a: Memory + the shared stat machinery** (`modules/mem/*`, the memory half of
    `sysmon/sysmon_client.c`, `util/history.h`, `render/{braille_graph,meter_bar,stat_card}.c`,
    `render/color.h`). The 5 s poll (calloop `Timer`), the 120-sample history ring, braille
    graphs, segmented meters, stat-card rows, left-anchored dropdowns (`PopupHost::open_left`),
    the memory box and its dropdown (PSI pill, used/swap graphs, apps/cache/free bar, RAM/SWAP
    meters, ZRAM/AVAIL/PRESS rows, top apps by private memory), and `dump mem` / `refresh mem|poll`.
    The memory half of C's `test_sysmon_data` (`check_memory`, `check_memory_apps`) passes
    against it, as do `test_popups`' `check_popup` for `mem` and `test_popup_graphs`'
    `graph_gap`. The dropdown is the same size and layout as C's (411×458 here).
  - [x] **M7b: CPU** (`modules/cpu/*`, the CPU half of `sysmon_client.c`). `/proc/stat` jiffies
    deltas (overall and per thread), package temperature, static facts (model, cores, max clock,
    P/E cpulists), the dropdown (load pill, usage/temp graphs, P-CORES/E-CORES grid, LOAD/CLOCK/
    UPTIME, top processes summed by name from a per-PID tick table), the 1 s "first figures"
    tick, and `dump cpu` / `refresh cpu|cpu-reset`. The CPU and memory dropdowns replace each
    other. **C's `test_sysmon_data` passes unchanged** (2 busy loops → `sh` 12.4 % of an expected
    12.5 %). The dropdown matches C's size and layout (416×553 here), and also passes
    `check_popup` and `graph_gap`.
  - [x] **M7c: GPU** (`modules/gpu/*`, NVML via `nvml-wrapper` 0.13, checked in its source to use
    the `_v2` memory call and the `_v3` process queries, loaded at runtime). The box (`--` without
    a driver), the dropdown (P-state pill, usage/temp graphs, VRAM/POWER/FAN meters, CLOCK/VIDEO,
    top VRAM apps), the unavailable state, and `dump gpu` / `refresh gpu`. The three sysmon dropdowns
    replace each other. C's `test_power_menu` (SIGUSR1 blocked in all 3 threads),
    `test_popup_graphs` and `test_no_subprocess` pass unchanged.
    **Verified only without a working driver:** during this step the NVIDIA kernel module (615.78)
    and userspace library (620.70) didn't match, which needs a reboot, so `nvidia-smi` itself
    failed. The unavailable state is identical to C's (same dropdown, 280×63). **Still to verify
    after a reboot:** `test_nvml_vs_smi`, the bar's `dump gpu` against `nvidia-smi`, and the
    GPU graph in `test_popup_graphs`.
- [x] **M8: Network** (`network/`, `modules/network/` ~760). Default route, rates, and the
  info dropdown with graph. Tests: `test_network_data`, `test_popups` (network). IPv4 via
  `nix`'s `getifaddrs`; one shared 5 s timer (`App::poll_5s`).
- [x] **M9: Volume + mic** (`audio/*`, `modules/{volume,mic}/` ~1.8k). The pulse thread,
  sink/source cache, scroll steps, device dropdown, mixer, and `target device|mixer`. No
  commits for no-op events. Tests: `test_audio`, `test_redraw` (pulse part), `test_popups`
  (volume/mixer/mic).
- [x] **M10: EQ** (`eq/*`, `modules/eq/*` ~800). state.json (serde_json), inotify (rustix) on
  the directory, the script launcher, the curve, the box and the dropdown, `dump eq` /
  `target eq` / `eq_cards`. **Never point a test at the real DAC script.** Both open EQ bugs
  fixed by following the script process instead of a timer: "applying" ends on the state write
  or the script's exit, and profile clicks are ignored while it runs. The script is started
  with `posix_spawn` and an empty signal mask (`Command` keeps the bar's blocked signals).
  **C's `test_eq` passes unchanged**, as does `test_popups` (EQ). The box and the dropdown are
  pixel-identical to C's in every state.
- [x] **M11: Tray** (`dbus/*`, `modules/tray/*` ~1.7k). The `dbus` thread (zbus 5, one async loop),
  the Watcher served by hand or Host registration, the item registry, IconPixmap → cairo
  (typed read, symbolic recolour), the PNG fallback, and the DBusMenu dropdown, opened when its
  entries arrive. `target trayicon|traymenu`, `tray_entries`. **C's `test_tray` passes unchanged**
  in Host mode and, under `dbus-run-session`, in Watcher mode. Icons and menus are
  pixel-identical to C's, real apps included.
- [x] **M12: Media** (`mpris/`, `modules/media/*` ~1.7k). Players on the `dbus` thread (shared
  connection, `PropertiesChanged` matched by owner), controls, asynchronous position polls, the
  duplicate hiding, the centred clamped box, and the dropdown with timeline seek and
  recreate-on-resize (no graph rows: M14). `dump media`, `target media`, `media_cards`. **C's
  `test_media` passes unchanged**, as does `test_popups` (media). Pixel-identical to C.
- [x] **M13: Privacy** (`pipewire/`, `modules/privacy/*` ~920). The `pipewire` thread
  (pipewire-rs 0.10), the node/link graph, mic/webcam/screen-share detection, the box and the
  dropdown. **C's `test_privacy` passes unchanged**; pixel-identical to C's (one known order
  difference inside a card). Playback-stream and Client tracking moves to M14, its only user.
- [ ] **M14: Visualizer** (`visualizer/*`, `bar/strip_surface.c` ~860). The desync
  subsurface, up to 4 capture streams only while the dropdown is open, FFT bands, and
  synth mode for tests. Tests: `test_visualizer`.
- [ ] **M15: Switch day.** Full `tests/run_all.py` green, `perf_compare --against main`
  table recorded, the Rust release build run as the daily bar for about a week. Then:
  `git branch c-legacy main`, merge `rust-rewrite` into `main` (a merge, not a squash),
  push all three branches, and point Hyprland's exec line at `target/release/stickBar`.

## Rust-only additions

Things the C bar never had, added during the port. They're not part of "parity with
the C bar", so the C bar's tests don't cover them; each has its own test, which skips
against the C bar.

- **Hand cursor over clickable things** (added after M4, at the user's request). Over
  anything that reacts to a click (any button), the pointer shows a hand; elsewhere
  the arrow. That covers bar boxes (the clock: its right-click opens the calendar) and
  dropdown targets (the calendar's chevrons and year text). It uses the
  `wp_cursor_shape_v1` protocol through SCTK's `CursorShapeManager`: the bar names the
  shape and Hyprland draws it from the user's cursor theme, with no images loaded and no
  `wayland-cursor`. The shape is sent on each pointer enter (tagged with its serial, which
  the protocol requires) and after that only when it changes, never per mouse move. Each
  module marks its boxes `clickable` in the `ModuleBox` it returns. Code:
  `src/wayland/pointer.rs` (`cursor_shape_at`, `update_cursor`). Test:
  `tests/test_cursor_shape.py`, which asks the bar for its decision through the
  `cursor`/`menu-cursor` commands, because test bars take no real input.

## Pieces to try writing yourself (optional, for learning Rust)
Small, self-contained, and covered by tests. Ask Claude to explain compiler errors, not to
write the code:
- the Hyprland event-line parser (M5), the `split_once(">>")` function from the earlier comparison;
- `/proc/meminfo` parsing for the memory module (M7);
- the kanji numeral lookup for workspaces (M5).

## Progress log
Each finished milestone gets a line here: date, commit, and a short perf_compare summary.
- 2026-11-11 · M0 · Cargo skeleton and lint policy (2236e2e). Release binary 286 KB.
- 2026-11-11 · M1 · Empty bar on every monitor. Both M1 tests pass; one commit per output,
  then none while idle. `perf_compare --against main` (60 s; the C bar runs every module,
  so this is a baseline, not a comparison): CPU 0.00 % vs 0.03 %, private memory 0.9 MB vs
  8.6 MB, RSS 7.3 MB vs 34.5 MB, 1 thread vs 5. Release binary 628 KB.
- 2026-11-11 · M2 · Test-control channel. `test_control_channel` passes against the Rust
  and C bars and fails with either bug put back (no input region; EOF not removed, which used
  200 ticks/2 s); M1 tests still pass. Idle cost unchanged from M1.
- 2026-11-11 · M3 · Clock. All 5 ported tests pass; clock box pixel-identical to C. One
  80 px commit per monitor per minute. `perf_compare --against main` (C runs every module):
  CPU 0.02 % vs 0.08 %, private 4.5 MB vs 8.7 MB, RSS 21.4 MB vs 35.1 MB. Threads 2 (pango's
  fontconfig thread, which inherits the blocked SIGUSR1: checked in `/proc/*/task/*/status`).
  Release binary 787 KB.
- 2026-11-11 · M4 · Pointer, popups, calendar. All 6 ported tests pass; calendar
  pixel-identical to C in 3 states. Opening it adds 3.8 MB RSS (C: 3.7). `perf_compare
  --against main` (C runs every module): CPU 0.02 % vs 0.10 %, private 4.5 MB vs 8.7 MB,
  RSS 21.6 MB vs 34.8 MB. Release binary 912 KB. The real pointer was not exercised by the
  tests (test bars take no input); it uses the same routing.
- 2026-11-11 · Rust-only · Hand cursor over clickable boxes and dropdown targets
  (`wp_cursor_shape_v1`). `test_cursor_shape` passes (skips on the C bar) and fails with the
  clock's `clickable` turned off. No new wakeups, so idle cost is unchanged.
- 2026-11-11 · M5 · Hyprland + workspaces. All 8 tests pass (17 unit tests);
  `test_workspaces` fails with the event filter broken. Pills pixel-identical to C.
  `perf_compare --against main` (C runs every module): CPU 0.03 % vs 0.05 %, private 5.7 MB
  vs 8.6 MB, RSS 25.5 MB vs 35.0 MB. Release binary 984 KB. A real workspace switch by
  clicking was not tested (the tests use the fake); it's left for the user to try.
- 2026-11-11 · M6 · Arch logo + power menu. All 9 tests pass (20 unit tests), including
  C's `test_power_menu` unchanged. Logo and overlay pixel-identical to C. `perf_compare
  --against main` (C runs every module): CPU 0.02 % vs 0.07 %, private 5.5 MB vs 8.6 MB,
  RSS 25.3 MB vs 35.2 MB. Release binary 1.1 MB. The real Logout/Restart/Shutdown calls are
  untested, as in C (the guard).
- 2026-11-11 · M7a · Memory + shared stat machinery. C's memory checks pass (400 MB child →
  404 MB); dropdown matches C's size and layout; all 9 tests pass (29 unit tests). Opening the
  dropdown adds about 2.36 MB RSS (C: 2.25), measured per memory region: 740 KB shm buffer
  (returned on close) plus fonts/libraries and heap, almost equal in both. A first measurement
  said 3.4 MB, but that was an artifact: an earlier open/snapshot in the same run was still
  counted. `perf_compare --against main` (C runs every module):
  CPU 0.05 % vs 0.05 % (both poll every 5 s now), private 5.6 MB vs 8.6 MB, RSS 25.6 MB vs 35.0 MB.
- 2026-11-11 · M7b · CPU. C's `test_sysmon_data` passes unchanged; all 10 tests and 33 unit
  tests pass; the dropdown matches C's. `perf_compare --against main` (C runs every module):
  CPU 0.08 % vs 0.10 %, private 5.6 MB vs 8.7 MB, RSS 25.9 MB vs 35.2 MB. Release binary 1.2 MB.
- 2026-11-11 · M7c · GPU via NVML. All tests pass (35 unit tests); GPU readings unverified
  until the driver mismatch is fixed by a reboot. Also fixed a race in `test_workspaces` (an event
  pushed before the bar had connected went nowhere). **Measure the release builds:**
  `perf_compare` defaults to the debug builds, and unoptimized Rust is much slower than
  unoptimized C (debug: Rust 0.13 % vs C 0.07 % CPU). Release vs release (`--bin
  target/release/stickBar --against ../StickBar/build-release/stickBar`): CPU 0.02 % vs 0.08 %,
  private 7.4 MB vs 8.7 MB, RSS 29.5 MB vs 35.8 MB, 3 threads vs 5. Release binary 1.4 MB.
- 2026-11-11 · M8 · Network. C's `test_network_data` passes unchanged, the network check from
  `test_popups` passes, all 13 tests and 38 unit tests pass; the dropdown matches C's
  (280x366). Release vs release: CPU 0.03 % vs 0.02 % (noise at this level), private 7.4 MB
  vs 8.3 MB, PSS 12.7 MB vs 15.6 MB, RSS 29.8 MB vs 35.2 MB, 3 threads vs 5 (C still runs
  every module). GPU readings still unverified (needs the reboot).
- 2026-11-11 · M9a · Pulse thread + volume/mic boxes. `dump audio` matches `pactl` for every
  sink/source (volume, mute, state, rate, bus, default, monitor); box widths equal C's; all 13
  tests pass, `test_redraw`'s PulseAudio no-op check now live (40 no-op events -> clock-only
  commits), SIGUSR1 blocked in all 4 threads. Release vs release: CPU 0.05 % vs 0.05 %,
  private 7.8 vs 8.3 MB, RSS 32.3 vs 35.0 MB, 4 threads vs 5.
- 2026-11-11 · M9b · Device dropdown. `test_audio`'s device checks pass unchanged (meter
  press/drag/scroll/mute on a null sink, mic list without monitors, card count); both
  dropdowns match C (364x312 and 364x224 here, same picture); a sink added or removed while
  open resizes it live (312 -> 400 -> 312 px). 43 unit tests.
- 2026-11-11 · M9c · Mixer. C's `test_audio` passes unchanged in full (devices and mixer:
  grows/shrinks with a `paplay` stream, slider and mute chip), and `test_popups` now runs every
  ported dropdown (10). The mixer matches C's (364x116 with one app). All 15 tests pass.
  Release vs release: CPU 0.05 % vs 0.07 %, private 7.9 vs 8.3 MB, PSS 14.1 vs 16.0 MB, RSS
  32.3 vs 35.2 MB, 4 threads vs 5.
- 2026-11-12 · M10 · EQ. C's `test_eq` and `test_popups` pass unchanged; 16 tests pass (47 unit
  tests). The box and dropdown are byte-identical to C's in 4 states (no state, active, off,
  custom) plus hovered and applying. Both BUG.md EQ bugs are fixed and checked with a failing fake
  script: "applying" clears when it exits, and a second click starts nothing. The script's
  `SigBlk` is 0. Release vs release: CPU 0.03 % vs 0.03 %, private 27.6 vs 28.0 MB, PSS 33.0 vs
  33.1 MB, RSS 53.9 vs 56.2 MB, 4 threads vs 5. Release binary 1.6 MB. Commits 39 vs 47/min, damaged pixels
  550k vs 532k/min. Memory is up for both bars since M9c, probably because NVML now loads (the
  driver works again after a reboot); not measured. Failing for reasons outside the bar:
  `test_sysmon_data` (a game and Java use more memory than its 400 MB process) and
  `test_nvml_vs_smi` (driver 620's `nvidia-smi` prints a fractional temperature that the test
  `int()`s). Both fail the same way against the C bar. A real card click against the DAC is
  left for the user.
- 2026-11-12 · M11 · Tray. C's `test_tray` passes in both modes; 17 tests pass (53 unit tests;
  the 5 failing are 3 unported modules plus `test_sysmon_data`/`test_nvml_vs_smi`, as at M10).
  Icon row, fake and real menus (Discord, Steam, CurseForge) byte-identical to C. A quit app's icon
  goes in 0.10 s in both modes (a C bar in Host mode never drops it: BUG.md). `test_calendar`
  caught a generic-`Value` read of a 256x256 pixmap (+6.1 MB): fixed with a typed visitor, which
  also cut debug RSS from 77.1 to 59.8 MB. zbus's executor runs on the `dbus` thread (one thread
  saved). Release vs release: CPU 0.12 % vs 0.12 % (a busy minute), private 28.1 vs 28.4 MB, PSS
  35.4 vs 35.6 MB, RSS 56.1 vs 58.1 MB, 7 threads vs 5 (`dbus`, plus zbus runtime's `async-io` and
  idle `blocking-1`). Release binary 3.1 MB (zbus adds 1.5 MB).
- 2026-11-12 · M12 · Media. C's `test_media` passes unchanged; 18 tests pass (58 unit tests; the
  4 failing are the 2 unported modules plus `test_sysmon_data`/`test_nvml_vs_smi`, as before). Box
  and dropdown byte-identical to C's with one and two fake players, except where C is wrong: it
  applied one player's `PropertiesChanged` to every player (BUG.md "Media"), which the Rust bar
  doesn't. No `Position` polls with the dropdown closed. Release vs release: CPU 0.02 % vs 0.03 %,
  private 28.1 vs 28.0 MB, PSS 36.9 vs 35.3 MB, RSS 56.2 vs 56.2 MB, 7 threads vs 5. Release
  binary 3.2 MB.
- 2026-11-12 · M13 · Privacy. C's `test_privacy` passes (a real Discord mic stream was
  running too, and was listed correctly); 19 tests pass (60 unit tests; failing: `test_visualizer`
  (M14), `test_sysmon_data`/`test_nvml_vs_smi` as before; `test_clock_alignment` failed once
  under load, average above 3, then passed 3/3). Box and dropdown pixel-identical to C's except
  the order of several streams in one card. 20 null-sink add/removes: 0 commits, under 10 ms CPU.
  Release vs release: CPU 0.05 % vs 0.05 %, private 28.8 vs 28.0 MB (PipeWire itself 0.2 MB),
  PSS 37.2 vs 34.3 MB, RSS 58.5 vs 56.2 MB, 8 threads vs 5. Release binary 3.3 MB.
