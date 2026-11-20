# The Rust port

The bar was ported from C to Rust one module at a time, milestones M0-M15, between
2026-11-11 and switch day, 2026-11-13. The C bar is kept on the `c-legacy` branch. This file
keeps why each crate was chosen, the things the Rust bar has that the C bar never did, and the
log of every milestone and change with its measurements.

## Crate choices
| Concern | C bar | Rust | Why |
|---|---|---|---|
| Event loop | hand-rolled epoll in `main.c` | **calloop**: one loop, every fd a source | It's what smithay-client-toolkit is built on, and it's safe |
| Wayland | raw libwayland + vendored XML | **smithay-client-toolkit** (layer-shell, shm, seat, output) + `wayland-protocols` (viewporter, subsurface), on the **system libwayland** (`wayland-backend` `client_system`) | Safe and maintained. Pin one SCTK version, since its API changes between releases. The system backend keeps `WAYLAND_DEBUG` in libwayland's wall-clock format, which the tests parse; the pure-Rust transport prints a different one |
| Drawing | cairo + pangocairo | **cairo-rs** (`png` feature), **pango**, **pangocairo** crates | The same C libraries behind safe wrappers |
| Draw → shm | cairo draws straight into the memfd | cairo draws into its own `ImageSurface`, then the bytes are copied into SCTK's `SlotPool` buffer | Drawing straight into the shm buffer needs `unsafe`. A copy is about 200 KB per bar frame, measured as negligible |
| Threads → main loop | eventfd (`util/notify_fd.c`) | **calloop `channel`**: worker threads send messages, the main loop wakes | Same pattern without raw fds |
| JSON | cJSON | **serde_json** | Hyprland IPC and the test-control replies |
| Hyprland IPC | own socket code | own code over `std::os::unix::net::UnixStream` | Small, and it keeps the classic→Lua dispatch quirk under our control. No async crate |
| SIGUSR1 | sigprocmask + signalfd | **calloop `signals`** source, created **first in `main`, before any thread starts** | Lesson from BUG.md "Power menu": NVML threads that didn't block SIGUSR1 killed the bar |
| D-Bus (tray, MPRIS) | sd-bus | **zbus 5** (async, on its own `dbus` thread → channels) | Pure Rust, no unsafe. zbus 6 dropped the blocking API; one async loop on one thread is simpler than several blocking iterators anyway |
| PulseAudio | libpulse threaded mainloop | **libpulse-binding** on its own thread → channel | Same threading model as the C bar |
| PipeWire | pw_thread_loop | **pipewire** crate (pipewire-rs) on its own thread → channel | Same |
| NVML | dlopen'd libnvidia-ml | **nvml-wrapper** | Loads the library at runtime, like the C bar's `dlopen`. Uses the `_v2` memory call ([modules/sysmon.md](modules/sysmon.md)) |
| Timers / clock | timerfd + localtime | calloop `Timer` + **chrono** (`Local`) | |
| inotify (EQ) | raw inotify | **rustix** `fs::inotify` as a calloop `Generic` source | rustix is already a dependency; its inotify API is safe, so no extra crate |
| getifaddrs (network) | libc | **nix** (`net` feature) | Safe wrapper |
| FFT (visualizer) | own `spectrum.c` | **realfft** (rustfft's real-input wrapper) | Does the half-size complex FFT + untangle C hand-wrote; byte-identical graphs |
| Child processes | fork/execv + mask reset | **nix `posix_spawn`** with an empty signal mask (`util::spawn`: the EQ script, updates, Proxmox, systemctl) | `std::process::Command` does **not** reset the mask, so its child keeps SIGINT/SIGTERM/SIGUSR1 blocked; `pre_exec` would need `unsafe` ([architecture.md](architecture.md), "Child processes") |

Add a crate only when the module that needs it is written, and keep `Cargo.lock` committed.

## Rust-only additions

Things the C bar never had, each with its own test or doc:

- **Hand cursor over clickable things** (after M4). Over anything that reacts to a click (any
  button) the pointer shows a hand, elsewhere the arrow: bar boxes and dropdown targets. It
  uses `wp_cursor_shape_v1` through SCTK's `CursorShapeManager`: the bar names the shape and
  Hyprland draws it from the user's cursor theme, with no images loaded. The shape is sent on
  each pointer enter (with its serial, which the protocol requires) and after that only when
  it changes, never per mouse move. Each module marks its boxes `clickable`; code in
  `src/wayland/pointer.rs` (`cursor_shape_at`, `update_cursor`). Test: `test_cursor_shape`,
  which asks the bar through the `cursor`/`menu-cursor` commands, since test bars take no real
  input.
- **The media dropdown as a Dashboard**, centred on the monitor: the EQ moved out of its own
  box into a card there, then the smart plugs joined it; the media box never disappears, so
  it's always reachable ([modules/media.md](modules/media.md), [modules/eq.md](modules/eq.md)).
- **A middle click on the media box skips to the next track.**
- **The Arch logo opens the power menu on right-click**, like every other box's menu.
- **Pending Arch updates** box and dropdown ([modules/updates.md](modules/updates.md)).
- **The Proxmox server box**, with Wake-on-LAN and shutdown ([modules/proxmox.md](modules/proxmox.md)).
- **Shelly smart plugs** in the media dropdown ([modules/shelly.md](modules/shelly.md)).

## Progress log
One line per milestone and per later change worth measuring: date, what, and the measurements.
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
- 2026-11-12 · M14a · Visualizer, analysis and drawing. C's `test_visualizer` passes unchanged
  (19 strip frames as a graph falls; C commits 37 for the same fall, more than one per tick).
  With the same steady synthetic tones the dropdown, graphs included, is byte-identical to C's.
  20 of 22 tests pass (the two environment ones fail as before). 64 unit tests. Outside synth
  mode the visualizer stays unavailable until M14b, so the daily path is unchanged.
- 2026-11-13 · M14b · Visualizer, real capture. Live with a fake player whose child plays a
  1 kHz L / 3 kHz R tone into a null sink: the graph followed that child's stream, each tone only
  in its half, the privacy dropdown didn't list the capture, and the node went on close and on
  pause. One playing card, dropdown open: 0.55 % CPU (C: 0.55 %); closed: 0.10 % (C: 0.20 %).
  20 of 22 tests pass (the 2 environment ones fail as before), 67 unit tests. Release binary
  4.4 MB. **Open item:** idle CPU in `perf_compare` reads 0.12 % vs C's 0.05 % today (load average
  about 4; at M13's commit both read 0.05 %). The M13 build measures 0.12 % too under the same
  conditions, so M14 adds nothing. All of it is the main thread (two 10 ms ticks in 20 s). To
  look into before switch day.
- 2026-11-13 · M15 part 1 · The trial started. The idle-CPU item from M14b is closed. Side by
  side for 60 s (two rounds) the bars use the same CPU (0.033 / 0.050 % each). Most of the old
  difference was tick-level noise on a loaded machine; the real finding was zbus's `blocking-1`
  thread waking every 500 ms forever (`blocking` 1.7 never retires a lone idle thread). It was
  spawned once, to connect the socket, so the `dbus` thread now opens the session-bus socket
  itself (`unix:path=` addresses; any other address falls back to zbus). One thread fewer, and
  wakeups 366–438/min against C's 483–791. `run_all.py` twice: 20/22, then 21/22. Only
  `test_nvml_vs_smi` failed both times (driver 620 output), and `test_sysmon_data` once (memory
  held by other apps); both fail against the C bar too. Release vs release, 60 s: CPU 0.05 vs
  0.03 % (3 vs 2 ticks), private 28.8 vs 28.0 MB, PSS 38.2 vs 34.8 MB, RSS 59.2 vs 56.3 MB, 7
  threads vs 5, commits and damage identical. Release binary 4.4 MB.
- 2026-11-13 · M15 part 2 · Switch day. The day's Rust-only work landed first (the EQ in the
  media dropdown, the idle media box, the Arch logo on right-click, the centred media
  dropdown, the Arch updates box). A side-by-side log of both release builds (no input, one
  monitor, sampled every minute, 2.9 h): own memory with swap 27.6 → 29.2 MB (C) and
  28.3 → 30.1 MB (Rust), flat after 1.5 h; RSS fell for both once the system started
  swapping, so swap has to be counted. CPU 0.040 % vs 0.050 %. `c-legacy` keeps the C bar;
  `rust-rewrite` merged into `main` (--no-ff); Hyprland runs `StickBar/target/release/stickBar`.
- 2026-11-20 · Rust-only · More performance lints, picked one by one from nursery/restriction
  (`missing_const_for_fn`, `needless_pass_by_ref_mut` and 7 more; the reasons for leaving out
  `suboptimal_flops` and `redundant_pub_crate` are in `Cargo.toml`). 13 functions became
  `const fn`, and 2 tray methods take `&self`. The release binary is the same size (4,572,616
  bytes), and release vs release for 60 s is level within noise (own CPU 0.07 vs 0.05 %, RSS
  59.3 vs 59.2 MB, commits and damage identical). The dev profile now keeps line tables only:
  debug binary 184 MB → 64 MB, a full rebuild of this crate 8.2 s → 5.2 s. LLD is already
  the linker (rustc's default since 1.90), so there's nothing to gain there. All 24 tests pass.
- 2026-11-20 · Rust-only · Confirmed by hand on the real bar: NVIDIA GPU readings, clicking
  workspace pills, Logout/Restart/Shutdown, and EQ profile switching on the real DAC (the
  items the log above left unverified). `systemctl` now starts through `util::spawn`
  (empty signal mask; found through PATH so `test_power_menu`'s fake still guards), which
  closes the last open bug. The docs were rewritten to describe only the Rust bar.

