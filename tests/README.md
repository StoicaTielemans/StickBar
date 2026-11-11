# Live tests

These tests run against a real Wayland session (Hyprland). Each one starts a
**second** stickBar instance next to whatever bar is already running and
observes it from the outside: `/proc`, `WAYLAND_DEBUG` output, and exit status.
Then it stops that instance. The tests don't patch or rebuild the source, so
they check the exact binary you run. They use only the Python standard library
(`test_media.py`'s and `test_tray.py`'s fakes also need PyGObject, and skip
without it).

The dropdown tests drive the bar through its **test-control channel**
(`src/testing/test_control.c`, enabled only by `STICKBAR_TEST_CONTROL=1`):
commands on stdin, one JSON reply per line on stdout. They inject clicks,
drags and scrolls into the bar's own pointer routing, read what its data
sources hold (`dump`), and render the open dropdown to a PNG (`snapshot`)
to check its pixels. **Your real cursor is never moved and nothing on
screen is clicked**, so these tests are safe to run while you use the
desktop:

- `STICKBAR_TEST_OUTPUT` keeps the test instance (its bar *and* its popups'
  click-catchers) on one monitor. `common.pick_test_output()` picks one
  without a fullscreen window, and the test skips if every monitor has one,
  so it never opens a popup over a game or a video.
- In test mode every surface gets an empty input region, so your real
  clicks pass straight through the test bar instead of closing its popups
  mid-test (seen while writing these: a stray real click closed the mixer
  under test).

```sh
./build.sh
python3 tests/run_all.py                 # all tests, ~2 min
python3 tests/test_redraw.py             # a single test
python3 tests/run_all.py --bin PATH      # test a different binary
```

Extra bars flash at the top of the screen while the tests run. Exit status: 0 is
pass, 1 is fail, 77 is skipped (for example, no NVIDIA driver).

| Test | Checks | Guards against |
|---|---|---|
| `test_calendar.py` | A right-click on the clock opens the calendar with 1 catcher, a mauve border and a mantle background. The menu surface has a buffer attached and committed before any other input. The chevrons and the year text navigate (next ≠ home, back = home, prev differs from both), hover highlights and clears, a left-click does nothing, and a catcher click closes it with 0 catchers left. Opening adds under 4 MB RSS, and a `wp_viewport.set_destination` appears. | A lost first paint (fails when the fix is removed), broken year navigation or hover, leaked or full-size catchers. Passes against the C bar too. |
| `test_control_channel.py` | On a test-control instance: `ping`, `outputs` (only the test monitor, with a bar and hyprctl's width), `rects` by name and index, an unknown command answered with an error. The bar surface gets an empty input region before its first commit. After stdin closes, the bar keeps running and stays idle. | A channel that stops answering, a test bar that eats the user's real clicks, a loop spinning on a closed stdin (200 ticks/2 s when put back). |
| `test_wayland_disconnect.py` | Runs the bar through `lib/wlproxy.py`, kills the proxy, and expects the bar to exit with status 1 within 3s. | Spinning at 100% CPU after a compositor crash or restart. |
| `test_clock_alignment.py` | With `STICKBAR_TEST_CLOCK_SECONDS` (seconds shown), the first commit of every second lands within 50ms of the second boundary (measured: under 1ms). | Clock seconds lagging real time by up to 1s. |
| `test_no_subprocess.py` | The bar spawns no child processes over 12s. It samples `/proc` every 3ms, which covers two 5s polls. | Running `nvidia-smi` every 5s again. |
| `test_redraw.py` | With `STICKBAR_TEST_CLOCK_SECONDS` (a clock tick every second): at least a quarter of commits (and at least 3) damage less than 2000px. 40 PulseAudio events that set the volume and mute to their current values cause no extra commits. | Full-bar redraws on every clock tick, and commits for frames that didn't change. |
| `test_stdin_intact.py` | After startup, fd 0 is still the stdin pipe the bar was given. | `shm_buffer.c` closing fd 0 for every surface: a zeroed `ShmBuffers` has `fd == 0`, and the old `fd >= 0` check closed it (stdin, or another surface's memfd). |
| `test_popups.py` | Each dropdown (CPU, GPU, memory, network, calendar, EQ, volume, mixer, mic, media) opens from an injected click with exactly one catcher, draws the module's accent border on a mantle background, and closes from a catcher click. The CPU/GPU/memory dropdowns replace each other; catchers use `wp_viewport`; an open dropdown adds under 4 MB RSS. | Leaked catchers, a dropdown that doesn't open or close, full-size catcher buffers (~8 MB per output). |
| `test_popup_graphs.py` | In a PNG of the open memory/CPU/GPU dropdown, the newest graph dots sit within 14 px of the card's right border. | The fixed 5-minute graph centred in a wider card (dots ended 47-77 px short). |
| `test_sysmon_data.py` | Memory (apps/free/available/shared/swap, PSI, zram) and CPU (model, threads, cores, max clock, P/E split, uptime) match `/proc` and sysfs. A 400 MB child shows up in the top apps at ≥ 95 % of that, and 2 busy loops show as `sh` at ~2/threads of the CPU. | Wrong meminfo fields, RSS-instead-of-private memory, a broken process-percent formula. |
| `test_network_data.py` | Interface, gateway, IP, MAC, MTU and DNS match `ip`/`/sys`/`resolv.conf`; rx/tx totals fall between the kernel counters read before and after; history grows one sample per poll. | Parsing regressions in `network_client.c`. |
| `test_audio.py` | On a throwaway null sink: a meter press sets ~75 %, a drag ends at ~25 %, scroll steps +5 %, the mute chip toggles. The mic dropdown lists only non-monitor sources. The mixer grows when a `paplay` stream starts, its slider and mute chip work, and it shrinks when the stream ends. Never touches a real device's volume or the default device. | Broken device-volume controls, monitors back in the mic list, a mixer that doesn't resize. |
| `test_media.py` | Against `lib/fake_mpris.py`: metadata (incl. album) and status are read right; the play/next/prev chips send PlayPause/Next/Previous; a click a quarter of the way along the timeline sends `SetPosition` with the track id and ~50 s of the 200 s track, and the dropdown stays open; the popup shrinks (or closes) when the player quits. | Broken controls or seeking, a popup stuck at its open-time size. |
| `test_visualizer.py` | **No real audio**: the bar's visualizer runs in synthetic mode (no PipeWire, no capture streams; tones set with `vis-tone`). Two silent `lib/fake_mpris.py` players, A with 1 kHz in the left channel only, B with 3 kHz in the right only. Dropdown closed, even while both play: every slot's levels stay zero. Open: each card's graph peaks at its own band in its own half only; a PNG shows A's tone column lit near the top of the left half, unlit under the centre line and at a quiet column; pausing A commits its falling graph as frames of its own subsurface and it settles to zero. Closed again: all zero. | Any visualizer work with the dropdown closed; the channels or cards mixed up; the graph missing, wrongly drawn or not animating on its subsurface; a paused graph frozen mid-pose. |
| `test_eq.py` | With a scratch `XDG_CACHE_HOME` and `lib/fake_eq_script.py` as `BLACKPEARL_EQ_SCRIPT` (the real script and the DAC are never touched): the active profile's card has the accent border and hovering lightens a card; a click runs `profile NAME`, the pill goes yellow, and the state write moves the border and turns the pill green at the same popup size; EQ off and no state file both give a grey pill. | A broken profile click path, a pill or border that doesn't follow the state. |
| `test_tray.py` | Against `lib/fake_tray.py`: the fake's icon appears; its menu shows every entry with access-key underscores stripped, checkmark/radio state and the submenu marker; 3 separators make 4 cards on a grey border; a disabled entry is inert; a click sends that entry's `clicked` event and closes the menu. | Raw `_` labels, lost toggle state, a broken click path. |
| `test_power_menu.py` | Every thread of the bar has SIGUSR1 in `SigBlk`; SIGUSR1 (the Hyprland keybind's signal, sent to the test instance's pid only) opens and closes the overlay, and a burst of them leaves the bar running; an Arch-logo click opens it, a background click closes it. The panel is centered on its surface; a hovered card takes its action's colour; a header click is absorbed; each card picks its own action. A test-control instance only records the action, and also has a fake `systemctl` and a dead Hyprland socket, so nothing can really run; the real calls aren't covered. | SIGUSR1 blocked only after NVML started its threads, so the keybind killed the bar (exit by signal 10). A panel centered by output height, not surface height. |
| `test_privacy.py` | A `parecord` on a throwaway null sink's monitor (no real mic): the box appears, the dropdown lists it under MICROPHONE with the maroon border and pill, a catcher click closes it, and the box collapses when the stream ends. Your own bar shows the indicator for those seconds too. | A dropdown missing its stream, a box that never collapses. |
| `test_nvml_vs_smi.py` | Makes the same NVML calls as `sysmon_client.c` and compares the results with `nvidia-smi`: usage, temperature, memory, fan, power and P-state, plus the GPU dropdown's name, driver, power limit, max clock and per-process VRAM list. | Wrong units, memory read through the v1 call (which counts reserved memory), or a wrong process-info struct layout. |

The tests were checked in both directions. Against the build from before these
fixes (commit `1814d4e`), every original test except the NVML one **fails**,
each for the bug it guards against. They all pass on the current build. The
NVML test checks the library calls themselves, not the bar, so it passes on
both builds.

The dropdown tests were checked the same way, by putting each bug back into
the current source, rebuilding and running the test, which then failed:
- the old `fd >= 0` close: `test_stdin_intact` (fd 0 became a shm memfd);
- a fixed 30-cell graph: `test_popup_graphs` (dots 47/77/23 px short);
- catchers without viewporter: `test_popups` (+8.2 MB);
- no monitor filter: `test_audio` (6 mic cards instead of 2);
- no media recreate: `test_media` (popup stuck at 300 px);
- no mixer recreate: `test_audio` (mixer stuck at 116 px);
- no accent border on the active profile card: `test_eq` (border stayed
  `surface1`);
- raw DBusMenu labels: `test_tray` ("Show Notifications" not found);
- the power panel centered by output height: `test_power_menu` (456 px above,
  416 below);
- SIGUSR1 blocked late, as before the fix: `test_power_menu` (the bar died
  of signal 10; an NVML thread, `cuda00001c00008`, didn't block it).

`test_audio`'s mixer check waits for its own stream by name and compares
the popup's drawn card count with its row count, instead of expecting the
count to change by exactly one: other apps' streams (a browser tab) come and
go during the test, which made the first version fail about one run in three.

`test_redraw.py` uses the real default sink, but it only ever sets the volume
and mute to the values they already have.

## Measuring performance

`perf_compare.py` measures performance and prints a table; it doesn't pass or fail.

```sh
python3 tests/perf_compare.py                         # target/debug/stickBar alone, 60s
python3 tests/perf_compare.py --against HEAD~1        # compare with a git ref (built in a temp worktree)
python3 tests/perf_compare.py --against ../other/stickBar --seconds 90
```

It reports:
- **CPU**, including spawned child processes.
- **Memory:** private, PSS and RSS.
- **Thread count.**
- **Compositor load:** commits and damaged pixels per minute.

The CPU/memory pass and the `WAYLAND_DEBUG` pass run separately, because debug
logging costs CPU. When comparing two builds, both start fresh at the same time.
A bar that has been running for hours shows a lower RSS, so only compare fresh
starts with fresh starts.

## Files in `lib/`

- `common.py`: starts and stops a bar instance, parses `WAYLAND_DEBUG` logs,
  and reads CPU and memory figures from `/proc`. `WAYLAND_DEBUG` timestamps are
  **UTC**, so convert wall-clock times with `gmtime()`.
- `common.py` also has `ControlledBar` (start a test-control instance, send
  commands, wait for the popup state), `pick_test_output()`, and a stdlib PNG
  reader (`png_pixels()`) with the Catppuccin colours to compare against.
- `fake_mpris.py`: an MPRIS player that only logs the calls it gets, so
  media tests never pause or skip a real one.
- `fake_tray.py`: a StatusNotifierItem with a DBusMenu that only logs
  clicks. It registers with the running watcher -- usually your own bar, so
  its grey icon shows in your tray for the ~20s the test runs; if none is
  running, the test's own bar instance takes the watcher role.
- `wlproxy.py`: a Unix-socket proxy that sits between the bar and the
  compositor. It also forwards the file descriptors Wayland passes over the
  socket (needed for shm buffers), so killing it simulates the compositor going
  away without touching your real session.
