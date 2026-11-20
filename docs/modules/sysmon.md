# System monitor (CPU/GPU/memory)

Files: `src/sysmon/mod.rs` (`Sysmon`: figures, histories, `poll()`), `src/sysmon/cpu.rs`,
`src/sysmon/gpu.rs`, `src/sysmon/memory.rs`, `src/sysmon/timer.rs` (the 5 s poll),
`src/util/history.rs` (`History2`), `src/render/{braille_graph,meter_bar,stat_card}.rs`, and
`src/modules/{cpu,gpu,mem}/{mod,popup}.rs`.

The reference bar has one combined `SystemMonitorWidget`/`SystemMonitor.qml` with a single
hover tooltip. At the user's request this bar has **three independent boxes** (peach CPU,
green GPU, yellow memory: the unused remainder of the Catppuccin palette), each with its
**own** right-click dropdown.

- **Polled every 5 s on the main thread** (a calloop `Timer`; the reference's
  `Timer { interval: 5000 }`). `App::poll_5s()` polls, refreshes an open sysmon dropdown,
  does the same for the network, and marks the bars dirty. No connection, no thread of its
  own: every poll re-reads `/proc/stat`, `/proc/meminfo` and
  `/sys/class/thermal/thermal_zone1/temp`, plus a few in-process NVML calls. The load averages
  and the `/proc/cpuinfo` clock average (28 KB, only a fallback when per-core `cpufreq` isn't
  exposed) are dropdown-only, read by `Cpu::refresh_details`. `Sysmon` lives in the
  `Renderer`, so bar boxes and dropdowns read it directly.
- **GPU stats come from NVML** through `nvml-wrapper`, which loads `libnvidia-ml.so.1` at
  runtime (the library `nvidia-smi` is a CLI over). No driver means the box shows `--`, the
  dropdown says "No NVIDIA GPU detected" instead of misleading zeros, and the bar still
  starts. This replaced running `nvidia-smi` every 5 s, which cost 15-45 ms a poll and blocked
  the loop. A `Device` borrows the `Nvml`, so it's looked up per call
  (`device_by_index(0)`, cheap) instead of being stored. NVML starts threads of its own; they
  start after the signal source, so they inherit the blocked SIGUSR1
  ([power-menu.md](power-menu.md)).
  - **The 5 s poll only reads what the box shows (usage + temperature, ~0.01 ms each).**
    VRAM, fan, power and P-state appear only in the dropdown, so `Gpu::refresh_details` reads
    them on open (with usage/temperature again, for one consistent snapshot). Measured: the
    fan query alone took 0.8-5.3 ms per call, ~90 % of the poll. Whole poll: ~0.7-1.5 ms;
    opening the dropdown pays ~4-11 ms once.
  - **Memory used is the `_v2` call** (`memory_info()`): v1's `used` includes
    driver/firmware-reserved memory (~300 MiB more here); v2's matches `nvidia-smi`'s
    `memory.used`. The process lists are `_v3`. The fan is `nvmlDeviceGetFanSpeed_v2` (fan 0).
  - **Tradeoff: ~25 MB more resident memory.** NVML pulls in `libcuda` and allocates its own
    buffers. Judged worth it for the CPU and latency win.
- **Histories (CPU, GPU, memory)** are pushed in `Sysmon::poll`, not in the GPU read the
  dropdown's refresh also calls, so it stays exactly one sample per 5 s.
- Dropdown sections take `Option<&cairo::Context>`: `None` only measures, which is how the
  height is fixed before the popup opens. Numbers from `/proc` are parsed straight to `f64`
  (exact below 2^53), which avoids lossy integer-to-float casts.

- **GPU dropdown** (`modules/gpu/popup.rs`). Top to bottom: header (short name with "NVIDIA GeForce" stripped, driver
  version, and a P-state pill: P0-2 `busy` peach, P3-7 `active` yellow,
  P8+ `idle` green); a graph card with braille usage (0-100%, green →
  yellow → red) and temperature (mapped 30-90 °C, peach → red);
  VRAM/power/fan as `render/meter_bar.rs` segmented meters; clock
  and encoder/decoder lines; and `TOP VRAM APPS` (the 5 biggest, a bar
  relative to the biggest, always 5 rows tall so the open-time size never
  clips).
  - **Detail NVML calls**, all optional (a failing call just leaves that
    field empty, never disables the module): name, driver version,
    enforced power limit and max clocks read **once** in `Gpu::new`;
    current clocks, encoder/decoder utilization and the process list in
    `Gpu::refresh_details` (dropdown only).
  - **Per-app VRAM**: `nvmlDeviceGetGraphicsRunningProcesses_v3` +
    `..._ComputeRunningProcesses_v3`, merged by PID keeping the
    larger figure (a C+G app is in both lists with the same number),
    `NVML_VALUE_NOT_AVAILABLE` counted as 0, named from `/proc/PID/comm`
    so a browser GPU process reads "helium", not its command line.
  - **Live while open**: `refresh_if_open` after the 5s poll re-runs
    `Gpu::refresh_details` (~4-11 ms, the fan query dominating) and repaints;
    nothing extra runs while it's closed.
  - Checked against `nvidia-smi` idle and under a `glxgears` load, and by the
    user on the real bar; `tests/test_nvml_vs_smi.py` compares every figure,
    including name, driver, power limit, max clock and the per-PID VRAM list.
- **CPU dropdown** (`modules/cpu/popup.rs`), the GPU dropdown's twin: same header
  / graph card / rows / top-list rhythm and the same `braille_graph`/`meter_bar`
  helpers.
  - **Header**: short model (`12th Gen Intel(R) Core(TM) ` stripped →
    `i5-12600K`; AMD's ` N-Core Processor` tail dropped), cores/threads
    from `/proc/cpuinfo` (`cpu cores`), and a load pill by overall usage
    (`idle` <15% green, `active` <60% yellow, `busy` peach).
  - **Per-thread grid**: meter + % + current clock (`cpufreq/
    scaling_cur_freq`, dropdown only), column-major like btop. **Hybrid
    Intel chips are split into P-CORES / E-CORES** using
    `/sys/devices/cpu_core/cpus` and `/sys/devices/cpu_atom/cpus` (kernel
    cpulist format, here `0-11` and `12-15`); without both files it's one
    `CORES` group.
  - **Rows**: load average + running/total tasks (4th `/proc/loadavg`
    field), average/max clock (`cpuinfo_max_freq`), uptime.
  - **Top processes** (`sysmon/cpu.rs`): one
    pass over `/proc/[0-9]*/stat` (`comm` runs from the first `(` to the
    *last* `)`, since names can contain either; utime+stime are fields
    14-15), diffed per PID against the previous pass (a `HashMap<pid, ticks>`
    replaced on every scan, so exited PIDs drop out), divided by elapsed ×
    ticks per second (`rustix::param::clock_ticks_per_second`) × thread
    count = % of the *whole* CPU, and
    **summed by name** so a browser's many processes are one line.
    Measured: 4 busy loops on 16 threads showed `sh 24.9%` (exactly 4/16).
    The scan runs only while the dropdown is open (~1-3 ms per refresh).
  - **First figures within a second**: the open-time scan only primes the
    table (reset first, so it never averages over however long the
    dropdown was closed); `CpuPopup::tick` on the clock's 1s timer does
    the second scan at the first tick ≥0.5s after opening (the clock fires
    on whole seconds, so the very next tick can be milliseconds away --
    seen live: a tick 0.3s after open was skipped, the next one filled it).
    After that it follows the 5s poll via `refresh_if_open`.
  - **Left out**: package power (RAPL's `energy_uj` is root-only) and
    per-core temperatures (coretemp reports per *physical* core; mapping
    threads to them wasn't worth it).
  - **Known quirk, kept**: overall usage counts
    iowait as busy (`idle` is only `/proc/stat`'s 4th field, matching the
    reference's `percentFromJiffies()`), so during heavy disk I/O it reads
    higher than `top`'s us+sy+ni -- seen during the load test: 62% vs ~28%
    with `wa` at ~25%.
  - Verified:
    model/cores/threads vs `lscpu`, P/E split vs sysfs, uptime vs
    `uptime -p`, clocks vs `/proc/cpuinfo`, and a 4-loop load.
- **Memory dropdown** (`modules/mem/popup.rs`), the CPU dropdown's twin in layout.
  - **Header**: total, swap kind (`zram swap` / `swap` / `no swap`), and a
    pill from **PSI** (`/proc/pressure/memory`, `some avg10`: `ok` <1%
    green, `pressure` <10% yellow, `thrashing` red), not from percent used
    -- RAM that looks full but is mostly cache isn't a problem, stalling
    is. Falls back to percent used (<70/<90) without PSI.
  - **Apps / cache / free bar**: apps = total - available (the box's own
    "used"), cache = available - free (reclaimable), free = MemFree; the
    three add up to the total, which explains a tiny "free" at a glance.
  - **Rows**: RAM/SWAP meters; ZRAM (only if `/proc/swaps` lists a
    `/dev/zramN`: active algorithm from the `[bracketed]` entry of
    `comp_algorithm`, stored -> compressed from `mm_stat`'s first two
    fields with the ratio, and its real RAM use from the third); AVAIL with
    shared (Shmem) and dirty; PRESS, the three PSI `some` averages.
  - **Top apps by *private* memory** (`sysmon/memory.rs`):
    `/proc/PID/statm` resident - shared (= RssAnon), summed by name like
    the CPU list. Measured on this desktop while choosing: summed **RSS**
    counts shared libraries once per process (helium 7.0 GB); **PSS** from
    `smaps_rollup` is exact (3.9 GB) but took ~215 ms and is unreadable
    for 328 of 439 processes (other users'); private memory gave 3.5 GB in
    ~6 ms (Python) and is readable for all. A snapshot, so unlike the CPU
    list it's ready at open -- no priming scan, no tick. The `stat`-name
    parsing is shared with the CPU scan (`proc_comm`).
  - The graph's peak labels start from the current (refreshed) values, so `used
    40%` never sits beside `peak 31%` when the fresh read is ahead of the last poll
    sample.
  - Verified:
    apps/free/avail vs `free -b`, zram vs `mm_stat`, PSI vs
    `/proc/pressure/memory`; with a 3 GB `bytearray` held by `python3`,
    apps rose 2.8 GB and `python3 3.00 GB` showed second in TOP APPS;
    `WAYLAND_DEBUG` showed the popup recommitting on the 5s poll.
- **CPU-jiffies delta computation matches the reference's own
  `percentFromJiffies()` exactly**: `/proc/stat`'s `cpuN` lines are parsed
  every poll, and each line's `(idle, total)` pair is diffed against the
  *previous* poll's sample for that same core (aggregate `cpu` line tracked
  separately from per-core `cpu0`, `cpu1`, ... lines) to get a percentage —
  which is why the very first poll after startup always
  reports 0% (no prior sample yet to diff against), self-correcting on the
  next poll 5 seconds later. Not a bug; matches the reference's own `prev ?
  percent : 0` behavior.
