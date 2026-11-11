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
| Wayland | raw libwayland + vendored XML | **smithay-client-toolkit** (layer-shell, shm, seat, output) + `wayland-protocols` (viewporter, subsurface) | Safe and maintained. Pin one SCTK version, since its API changes between releases |
| Drawing | cairo + pangocairo | **cairo-rs** (`png` feature), **pango**, **pangocairo** crates | The same C libraries behind safe wrappers |
| Draw → shm | cairo draws straight into the memfd | cairo draws into its own `ImageSurface`, then the bytes are copied into SCTK's `SlotPool` buffer | Drawing straight into the shm buffer needs `unsafe`. A copy is about 200 KB per bar frame. Measure it in M1 |
| Threads → main loop | eventfd (`util/notify_fd.c`) | **calloop `channel`**: worker threads send messages, the main loop wakes | Same pattern without raw fds |
| JSON | cJSON | **serde_json** | Hyprland IPC and the test-control replies |
| Hyprland IPC | own socket code | own code over `std::os::unix::net::UnixStream` | Small, and it keeps the classic→Lua dispatch quirk under our control. No async crate |
| SIGUSR1 | sigprocmask + signalfd | **calloop `signals`** source, created **first in `main`, before any thread starts** | Lesson from BUG.md "Power menu": NVML threads that didn't block SIGUSR1 killed the bar |
| D-Bus (tray, MPRIS) | sd-bus | **zbus** (blocking API on its own thread → channel) | Pure Rust, no unsafe |
| PulseAudio | libpulse threaded mainloop | **libpulse-binding** on its own thread → channel | Same threading model as the C bar |
| PipeWire | pw_thread_loop | **pipewire** crate (pipewire-rs) on its own thread → channel | Same |
| NVML | dlopen'd libnvidia-ml | **nvml-wrapper** | Loads the library at runtime, like now. Use the v2 memory call (BUG.md lesson) |
| Timers / clock | timerfd + localtime | calloop `Timer` + **chrono** (`Local`) | |
| inotify (EQ) | raw inotify | **inotify** crate as a calloop source | |
| getifaddrs (network) | libc | **nix** (`net` feature) | Safe wrapper |
| FFT (visualizer) | own `spectrum.c` | **rustfft** | |
| Child processes | fork/execv + mask reset | `std::process::Command` | std already resets the signal mask in the child, which the C bar does by hand in `eq_state.c` |

Add each crate only in the milestone that first needs it.

## Milestones
Each lists: the C files to read on `main` (rough line count), what to build, and which tests
must pass.

- [x] **M0: Skeleton.** Cargo project, lint policy, `build.sh`, tests pointed at `target/`.
- [ ] **M1: An empty bar on every monitor** (~2.6k C lines: `main.c`, `wayland/*`,
  `bar/bar_surface.c`, `config/*`, `render/render.c`, `render/module_box.c`, `util/*`).
  Connect, track outputs and their `index` by binding order, create one 20 px top layer
  surface per output with an exclusive zone, double-buffered shm, frame-callback-throttled
  redraw with dirty flags and minimal damage, colours, fonts and glyph consts, and the pango
  text helpers with the layout cache. Exit with status 1 when the compositor goes away.
  Tests: `test_wayland_disconnect`, `test_stdin_intact`.
- [ ] **M2: Test-control channel + pointer** (`testing/test_control.c` ~665,
  `wayland/pointer.c`, `render/box_rects.c`). The `STICKBAR_TEST_CONTROL` stdin channel,
  `STICKBAR_TEST_OUTPUT`, empty input regions in test mode, `rects`/`click`, and button/motion/scroll
  routing through one dispatch path. Built as a command table that later milestones add
  to. *Every later test depends on this, so it comes before any module.*
- [ ] **M3: Clock** (`modules/clock/clock.c`). The box, repainting only on the minute,
  `STICKBAR_TEST_CLOCK_SECONDS`. Tests: `test_clock_alignment`, `test_redraw` (clock
  part).
- [ ] **M4: Popup infrastructure + year calendar** (`bar/popup_surface.c`,
  `bar/popup_host.c`, `render/popup_chrome.c`, `render/popup_style.h`,
  `modules/clock/clock_calendar_popup.c`). Transient layer surfaces, catchers with
  `wp_viewport`, anchoring under the box, open/close/resize, and the `menu`/`snapshot`/`catcher-click`
  commands. Read `docs/popups.md` and BUG.md "Popups" first. Tests: `test_popups`
  (calendar part).
- [ ] **M5: Hyprland + workspaces** (`hyprland/*`, `modules/workspaces/` ~560). The request
  socket, event socket line framing, click-to-focus with the classic→Lua fallback, and kanji pills
  per 5-wide output range. Cross-check against `hyprctl -j workspaces`. Read BUG.md
  "Hyprland IPC".
- [ ] **M6: Arch logo + power menu + SIGUSR1** (`modules/powermenu/*` ~450). This milestone is
  placed **before** any milestone that starts a thread, so the signal-first ordering is
  proven while the bar has a single thread. Keep the test guard (record-only actions). Tests:
  `test_power_menu`.
- [ ] **M7: CPU / memory / GPU** (`sysmon/sysmon_client.c` ~1.1k, `modules/{cpu,mem,gpu}/`,
  `render/{braille_graph,meter_bar,stat_card}.c`; ~2.4k). The shared 5 s poll, /proc parsing,
  NVML, the three dropdowns, graphs and meters, and `refresh`/`dump` commands. Tests:
  `test_sysmon_data`, `test_popup_graphs`, `test_nvml_vs_smi`, `test_no_subprocess`,
  `test_popups` (cpu/mem/gpu).
- [ ] **M8: Network** (`network/`, `modules/network/` ~760). Default route, rates, and the
  info dropdown with graph. Tests: `test_network_data`, `test_popups` (network).
- [ ] **M9: Volume + mic** (`audio/*`, `modules/{volume,mic}/` ~1.8k). The pulse thread,
  sink/source cache, scroll steps, device dropdown, mixer, and `target device|mixer`. No
  commits for no-op events. Tests: `test_audio`, `test_redraw` (pulse part), `test_popups`
  (volume/mixer/mic).
- [ ] **M10: EQ** (`eq/*`, `modules/eq/*` ~800). state.json, inotify, script launcher, curve,
  dropdown. **Never point a test at the real DAC script.** Fix the two open EQ bugs from
  BUG.md as part of the port (a pending-state timer tick, and ignoring clicks while a profile
  is applying). Tests: `test_eq`, `test_popups` (EQ).
- [ ] **M11: Tray** (`dbus/*`, `modules/tray/*` ~1.7k). zbus thread, become the
  StatusNotifierWatcher or fall back to host registration, item registry, IconPixmap →
  cairo, icon-theme fallback, DBusMenu context menu. Tests: `test_tray`.
- [ ] **M12: Media** (`mpris/`, `modules/media/*` ~1.7k). Player table on the shared zbus
  connection, controls, position, duplicate hiding, the centered box, and the dropdown with
  timeline seek and recreate-on-resize. Read BUG.md "Media". Tests: `test_media`,
  `test_popups` (media).
- [ ] **M13: Privacy** (`pipewire/`, `modules/privacy/*` ~920). The PipeWire thread, the
  node/link/client graph, and mic/webcam/screen-share detection. Tests: `test_privacy`.
- [ ] **M14: Visualizer** (`visualizer/*`, `bar/strip_surface.c` ~860). The desync
  subsurface, up to 4 capture streams only while the dropdown is open, FFT bands, and
  synth mode for tests. Tests: `test_visualizer`.
- [ ] **M15: Switch day.** Full `tests/run_all.py` green, `perf_compare --against main`
  table recorded, the Rust release build run as the daily bar for about a week. Then:
  `git branch c-legacy main`, merge `rust-rewrite` into `main` (a merge, not a squash),
  push all three branches, and point Hyprland's exec line at `target/release/stickBar`.

## Pieces to try writing yourself (optional, for learning Rust)
Small, self-contained, and covered by tests. Ask Claude to explain compiler errors, not to
write the code:
- the Hyprland event-line parser (M5), the `split_once(">>")` function from the earlier comparison;
- `/proc/meminfo` parsing for the memory module (M7);
- the kanji numeral lookup for workspaces (M5).

## Progress log
Each finished milestone gets a line here: date, commit, and a short perf_compare summary.
- 2026-11-11 · M0 · Cargo skeleton and lint policy (2236e2e). Release binary 286 KB.
