# Live tests

These tests run against a real Wayland session (Hyprland). Each one starts a
**second** stickBar instance next to whatever bar is already running and
observes it from the outside: `/proc`, `WAYLAND_DEBUG` output, and exit status.
Then it stops that instance. The tests don't patch or rebuild the source, so
they check the exact binary you run. They use only the Python standard library
(`test_media.py`'s fake player also needs PyGObject, and skips without it).

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
meson compile -C build
python3 tests/run_all.py                 # all tests, ~2 min
python3 tests/test_redraw.py             # a single test
python3 tests/run_all.py --bin PATH      # test a different binary
```

Extra bars flash at the top of the screen while the tests run. Exit status: 0 is
pass, 1 is fail, 77 is skipped (for example, no NVIDIA driver).

| Test | Checks | Guards against |
|---|---|---|
| `test_wayland_disconnect.py` | Runs the bar through `lib/wlproxy.py`, kills the proxy, and expects the bar to exit with status 1 within 3s. | Spinning at 100% CPU after a compositor crash or restart. |
| `test_clock_alignment.py` | The first commit of every second lands within 50ms of the second boundary (measured: under 1ms). | Clock seconds lagging real time by up to 1s. |
| `test_no_subprocess.py` | The bar spawns no child processes over 12s. It samples `/proc` every 3ms, which covers two 5s polls. | Running `nvidia-smi` every 5s again. |
| `test_redraw.py` | At least a quarter of commits (and at least 3) damage less than 2000px. 40 PulseAudio events that set the volume and mute to their current values cause no extra commits. | Full-bar redraws on every clock tick, and commits for frames that didn't change. |
| `test_stdin_intact.py` | After startup, fd 0 is still the stdin pipe the bar was given. | `shm_buffer.c` closing fd 0 for every surface: a zeroed `ShmBuffers` has `fd == 0`, and the old `fd >= 0` check closed it (stdin, or another surface's memfd). |
| `test_popups.py` | Each dropdown (CPU, GPU, memory, network, calendar, volume, mixer, mic, media) opens from an injected click with exactly one catcher, draws the module's accent border on a mantle background, and closes from a catcher click. The CPU/GPU/memory dropdowns replace each other; catchers use `wp_viewport`; an open dropdown adds under 4 MB RSS. | Leaked catchers, a dropdown that doesn't open or close, full-size catcher buffers (~8 MB per output). |
| `test_popup_graphs.py` | In a PNG of the open memory/CPU/GPU dropdown, the newest graph dots sit within 14 px of the card's right border. | The fixed 5-minute graph centred in a wider card (dots ended 47-77 px short). |
| `test_sysmon_data.py` | Memory (apps/free/available/shared/swap, PSI, zram) and CPU (model, threads, cores, max clock, P/E split, uptime) match `/proc` and sysfs. A 400 MB child shows up in the top apps at ≥ 95 % of that, and 2 busy loops show as `sh` at ~2/threads of the CPU. | Wrong meminfo fields, RSS-instead-of-private memory, a broken process-percent formula. |
| `test_network_data.py` | Interface, gateway, IP, MAC, MTU and DNS match `ip`/`/sys`/`resolv.conf`; rx/tx totals fall between the kernel counters read before and after; history grows one sample per poll. | Parsing regressions in `network_client.c`. |
| `test_audio.py` | On a throwaway null sink: a meter press sets ~75 %, a drag ends at ~25 %, scroll steps +5 %, the mute chip toggles. The mic dropdown lists only non-monitor sources. The mixer grows when a `paplay` stream starts, its slider and mute chip work, and it shrinks when the stream ends. Never touches a real device's volume or the default device. | Broken device-volume controls, monitors back in the mic list, a mixer that doesn't resize. |
| `test_media.py` | Against `lib/fake_mpris.py`: metadata (incl. album) and status are read right; the play/next/prev chips send PlayPause/Next/Previous; the popup shrinks (or closes) when the player quits. | Broken controls, a popup stuck at its open-time size. |
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
- no media recreate: `test_media` (popup stuck at 300 px).

`test_redraw.py` uses the real default sink, but it only ever sets the volume
and mute to the values they already have.

## Measuring performance

`perf_compare.py` measures performance and prints a table; it doesn't pass or fail.

```sh
python3 tests/perf_compare.py                         # build/stickBar alone, 60s
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
- `wlproxy.py`: a Unix-socket proxy that sits between the bar and the
  compositor. It also forwards the file descriptors Wayland passes over the
  socket (needed for shm buffers), so killing it simulates the compositor going
  away without touching your real session.
