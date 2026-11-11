# System monitor (CPU/GPU/memory)

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
  `/proc/meminfo` and `/sys/class/thermal/thermal_zone1/temp` from scratch (small
  pseudo-files; single-value ones via `util/file.c`'s open/read, no stdio buffer)
  plus a handful of in-process NVML calls for the GPU. The load averages and
  the `/proc/cpuinfo` clock average (28 KB, only a fallback for when per-core
  `cpufreq` isn't exposed) are dropdown-only, read in
  `sysmon_cpu_refresh_details()`
  (see below). No locking anywhere in this file, unlike `pulse_client.c` —
  everything runs on the main thread, so there's no concurrent writer to
  guard against. (NVML starts two idle threads of its own inside the
  library; nothing here touches them.)
- **GPU stats come from NVML (`libnvidia-ml.so.1`), loaded with `dlopen()`
  at startup** -- the library `nvidia-smi` is itself a CLI over. This replaced an
  earlier `popen("nvidia-smi ...")` every 5 seconds, which blocked the event loop
  (costs in [architecture.md](../architecture.md)).
  - **The 5s poll only reads what the bar box shows (usage + temperature,
    ~0.01ms each).** VRAM, fan, power and P-state appear only in the
    dropdown, so `gpu_handle_click()` fetches them on open via
    `sysmon_gpu_refresh_details()` (which also re-reads usage/temperature for
    one consistent snapshot). Reason: measured in-bar, the fan-speed query
    alone took 0.8-5.3ms *per call* -- ~90% of the poll -- while everything
    else was ~0.01ms warm. Whole sysmon poll went from ~3.4-5.8ms to
    ~0.7-1.5ms; opening the dropdown pays ~4-11ms once, and shows live values
    instead of ones up to 5s old.
  - **`dlopen`, not linking**: `nvml.h` only ships with the CUDA toolkit
    (`/opt/cuda`), and linking would make stickBar fail to start without the
    NVIDIA driver. The handful of types/prototypes needed are declared
    locally in `sysmon_client.c`, using NVML's ABI-stable versioned symbols.
  - **Memory used is `nvmlDeviceGetMemoryInfo_v2`, not the v1 call**: v1's
    `used` includes driver/firmware-reserved memory (~300 MiB more here);
    v2's matches `nvidia-smi`'s own `memory.used` — confirmed side by side.
  - **Tradeoff: ~25 MB more resident memory.** NVML pulls in `libcuda` and allocates
    its own buffers (measured by building the same tree with NVML disabled). Judged
    worth it for the CPU and latency win; if memory ever matters more, this is where
    it went.
  - If the library or driver is missing, `sysmon_gpu_available()` stays false,
    the GPU module shows `--` instead of a percent, and its dropdown shows an
    explicit "No NVIDIA GPU detected" message instead of stale/zero-looking
    numbers — the same "explicit unavailable state, not a misleading zero"
    instinct behind `pulse_client.c`'s accessors returning safe defaults when
    disconnected.
- **History (CPU, GPU, memory)**: ring buffers pushed in `sysmon_client_poll()` --
  not inside `poll_gpu()`, which the dropdown's refresh also calls, so it stays
  exactly one sample per 5s.
- **GPU dropdown** (`modules/gpu/gpu_popup.c`). Each module used to share a text
  block in `sysmon_info_popup.c`, deleted once memory moved over too (comments that
  cited it as the TOP+LEFT example now point at `cpu_popup.c`).
  Top to bottom: header (short name with "NVIDIA GeForce" stripped, driver
  version, and a P-state pill: P0-2 `busy` peach, P3-7 `active` yellow,
  P8+ `idle` green); a graph card with braille usage (0-100%, green →
  yellow → red) and temperature (mapped 30-90 °C, peach → red);
  VRAM/power/fan as `render/meter_bar.c` segmented meters; clock
  and encoder/decoder lines; and `TOP VRAM APPS` (the 5 biggest, a bar
  relative to the biggest, always 5 rows tall so the open-time size never
  clips).
  - **New NVML calls**, all optional (a missing symbol just leaves that
    field empty, never disables the module): name, driver version,
    enforced power limit and max clocks read **once** in `nvml_init()`;
    current clocks, `nvmlDeviceGetEncoder/DecoderUtilization` and the
    process list in `sysmon_gpu_refresh_details()` (dropdown only).
  - **Per-app VRAM**: `nvmlDeviceGetGraphicsRunningProcesses_v3` +
    `..._ComputeRunningProcesses_v3` (struct = `nvmlProcessInfo_v2_t`,
    checked against `/opt/cuda`'s `nvml.h`), merged by PID keeping the
    larger figure (a C+G app is in both lists with the same number),
    `NVML_VALUE_NOT_AVAILABLE` counted as 0, named from `/proc/PID/comm`
    so a browser GPU process reads "helium", not its command line.
  - **Live while open**: `gpu_popup_refresh_if_open()` after the 5s poll
    re-runs `sysmon_gpu_refresh_details()` (~4-11 ms, the fan query
    dominating) and repaints; nothing extra runs while it's closed.
  - Verified with real `ydotool` right-clicks on a second bar instance (as were the
    CPU and memory dropdowns below):
    idle values matched `nvidia-smi` (name, driver, 1.7/10.0 GB, 340 W,
    P5); under a vsync-off `glxgears` load the popup showed 95%, `P0
    busy`, 167 W, fan 57%, 1935 MHz against `nvidia-smi`'s 82%/166 W/
    1935 MHz/P0 at nearly the same moment; `WAYLAND_DEBUG` showed the
    popup surface recommitting every 5s while open.
    `tests/test_nvml_vs_smi.py` now also checks name, driver, power
    limit, max clock and the per-PID VRAM list.
- **CPU dropdown** (`modules/cpu/cpu_popup.c`), the GPU dropdown's twin: same header
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
  - **Top processes** (`sample_cpu_processes()` in `sysmon_client.c`): one
    pass over `/proc/[0-9]*/stat` (`comm` runs from the first `(` to the
    *last* `)`, since names can contain either; utime+stime are fields
    14-15), diffed per PID against the previous pass (two swapped
    open-addressing tables keyed by PID, so exited PIDs drop out), divided
    by elapsed × `_SC_CLK_TCK` × thread count = % of the *whole* CPU, and
    **summed by name** so a browser's many processes are one line.
    Measured: 4 busy loops on 16 threads showed `sh 24.9%` (exactly 4/16).
    The scan runs only while the dropdown is open (~1-3 ms per refresh).
  - **First figures within a second**: the open-time scan only primes the
    table (reset first, so it never averages over however long the
    dropdown was closed); `cpu_popup_tick()` on the 1s clock timer does
    the second scan at the first tick ≥0.5s after opening (the clock fires
    on whole seconds, so the very next tick can be milliseconds away --
    seen live: a tick 0.3s after open was skipped, the next one filled it).
    After that it follows the 5s poll via `cpu_popup_refresh_if_open()`.
  - **Left out**: package power (RAPL's `energy_uj` is root-only) and
    per-core temperatures (coretemp reports per *physical* core; mapping
    threads to them wasn't worth it).
  - **Known quirk, pre-existing, not changed**: overall usage counts
    iowait as busy (`idle` is only `/proc/stat`'s 4th field, matching the
    reference's `percentFromJiffies()`), so during heavy disk I/O it reads
    higher than `top`'s us+sy+ni -- seen during the load test: 62% vs ~28%
    with `wa` at ~25%.
  - Verified:
    model/cores/threads vs `lscpu`, P/E split vs sysfs, uptime vs
    `uptime -p`, clocks vs `/proc/cpuinfo`, and a 4-loop load.
- **Memory dropdown** (`modules/mem/mem_popup.c`), the CPU dropdown's twin in layout.
  - **Header**: total, swap kind (`zram swap` / `swap` / `no swap`), and a
    pill from **PSI** (`/proc/pressure/memory`, `some avg10`: `ok` <1%
    green, `pressure` <10% yellow, `thrashing` red), not from percent used
    -- RAM that looks full but is mostly cache isn't a problem, stalling
    is. Falls back to percent used (<70/<90) without PSI.
  - **Apps / cache / free bar**: apps = total - available (the box's own
    "used"), cache = available - free (reclaimable), free = MemFree; the
    three add up to the total, which explains a tiny "free" at a glance.
    Note `sysmon_mem_free_gb()` used to return *MemAvailable* (the old
    text dropdown called that "FREE"); it's now real MemFree, and
    available is `sysmon_mem_avail_gb()`.
  - **Rows**: RAM/SWAP meters; ZRAM (only if `/proc/swaps` lists a
    `/dev/zramN`: active algorithm from the `[bracketed]` entry of
    `comp_algorithm`, stored -> compressed from `mm_stat`'s first two
    fields with the ratio, and its real RAM use from the third); AVAIL with
    shared (Shmem) and dirty; PRESS, the three PSI `some` averages.
  - **Top apps by *private* memory** (`sample_mem_processes()`):
    `/proc/PID/statm` resident - shared (= RssAnon), summed by name like
    the CPU list. Measured on this desktop while choosing: summed **RSS**
    counts shared libraries once per process (helium 7.0 GB); **PSS** from
    `smaps_rollup` is exact (3.9 GB) but took ~215 ms and is unreadable
    for 328 of 439 processes (other users'); private memory gave 3.5 GB in
    ~6 ms (Python) and is readable for all. A snapshot, so unlike the CPU
    list it's ready at open -- no priming scan, no tick. The `stat`-name
    parsing is shared with the CPU scan (`read_proc_stat()`).
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
  which is why the very first poll after `sysmon_client_init()` always
  reports 0% (no prior sample yet to diff against), self-correcting on the
  next poll 5 seconds later. Not a bug; matches the reference's own `prev ?
  percent : 0` behavior.

## In the Rust port

- **Files:** `src/sysmon/mod.rs` (`Sysmon`: figures + histories, `poll()`),
  `src/sysmon/memory.rs`, `src/sysmon/timer.rs` (the 5 s calloop `Timer`),
  `src/util/history.rs` (`History2`), `src/render/{braille_graph,meter_bar,stat_card}.rs`,
  `src/modules/mem/{mod,popup}.rs`, `src/sysmon/cpu.rs`, `src/modules/cpu/{mod,popup}.rs`. GPU
  (M7c) joins `Sysmon` the same way.
- **CPU process sample:** the per-PID tick table is a `HashMap<pid, ticks>` replaced on every scan,
  so exited PIDs drop out (C used two swapped open-addressing tables). Ticks per second come
  from `rustix::param::clock_ticks_per_second`. The first-figures tick hangs off the clock's
  1 s timer (`App::clock_tick` → `CpuPopup::tick`).
- `Sysmon` lives in the `Renderer`, next to the other module data, so bar boxes and dropdowns
  read it directly. `App::sysmon_poll()` polls, refreshes an open sysmon dropdown and marks the
  bars dirty.
- Dropdown sections take `Option<&cairo::Context>`: `None` only measures. That's how the height
  is fixed before the popup opens (C's `paint(NULL)` pass).
- Numbers from `/proc` are parsed straight to `f64` (exact below 2^53), which avoids lossy
  integer-to-float casts.
