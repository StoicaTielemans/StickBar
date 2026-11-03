# Live tests

These tests run against a real Wayland session (Hyprland). Each one starts a
**second** stickBar instance next to whatever bar is already running and
observes it from the outside: `/proc`, `WAYLAND_DEBUG` output, and exit status.
Then it stops that instance. The tests don't patch or rebuild the source, so
they check the exact binary you run. They use only the Python standard library.

```sh
meson compile -C build
python3 tests/run_all.py                 # all tests, ~40s
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
| `test_redraw.py` | Most commits damage less than 2000px. 40 PulseAudio events that set the volume and mute to their current values cause no extra commits. | Full-bar redraws on every clock tick, and commits for frames that didn't change. |
| `test_nvml_vs_smi.py` | Makes the same NVML calls as `sysmon_client.c` and compares the results with `nvidia-smi`. | Wrong units, or memory read through the v1 call, which counts reserved memory. |

The tests were checked in both directions. Against the build from before these
fixes (commit `1814d4e`), every test except the NVML one **fails**, each for
the bug it guards against. They all pass on the current build. The NVML test
checks the library calls themselves, not the bar, so it passes on both builds.

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
- `wlproxy.py`: a Unix-socket proxy that sits between the bar and the
  compositor. It also forwards the file descriptors Wayland passes over the
  socket (needed for shm buffers), so killing it simulates the compositor going
  away without touching your real session.
