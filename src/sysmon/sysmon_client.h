#ifndef CUSTOMBAR_SYSMON_SYSMON_CLIENT_H
#define CUSTOMBAR_SYSMON_SYSMON_CLIENT_H

#include <stdbool.h>

// Establishes the /proc/stat CPU-jiffies baseline this needs before it can
// compute a real delta-based percentage -- call once at startup, before the
// event loop begins (same "populate an initial snapshot" convention
// workspaces_refresh()/clock_tick() already follow in main.c). The very
// first sysmon_client_poll() after this will report 0% CPU (no prior sample
// to diff against yet), matching the reference SystemMonitor.qml's own
// `prev ? percent : 0` behavior -- not a bug, corrects itself on the next
// poll 5 seconds later.
//
// Also loads NVIDIA's NVML library (libnvidia-ml, via dlopen) for GPU stats;
// if it's missing, the GPU stays "unavailable" for the run.
void sysmon_client_init(void);

// Releases NVML (if it was loaded). Call once at teardown.
void sysmon_client_shutdown(void);

// Re-reads /proc/stat, /proc/meminfo, /sys/class/thermal/thermal_zone1/temp,
// /proc/cpuinfo, /proc/loadavg, and the GPU's stats via in-process NVML calls
// (no subprocess -- see CLAUDE.md's "System monitor (CPU/GPU/memory)"
// section). Call once at startup and then on every tick of a 5-second timerfd that main.c owns and drives directly,
// same convention as the clock's own timerfd -- no epoll-fd ownership is
// needed here, unlike audio/pulse_client.c or dbus/dbus_loop.c, since
// nothing in this module has state pushed to it asynchronously between polls.
void sysmon_client_poll(void);

// --- CPU ---
double sysmon_cpu_percent(void);
int sysmon_cpu_temp_c(void);
double sysmon_cpu_freq_ghz(void);
const char *sysmon_cpu_load_avg(void); // "N.NN N.NN N.NN" (1/5/15 min), stable buffer
int sysmon_cpu_core_count(void);
double sysmon_cpu_core_percent(int index); // 0 <= index < sysmon_cpu_core_count()

// Static, read once at init ("" / 0 if unknown).
const char *sysmon_cpu_model(void); // e.g. "12th Gen Intel(R) Core(TM) i5-12600K"
int sysmon_cpu_physical_cores(void);
double sysmon_cpu_max_ghz(void);
// True on hybrid Intel chips (P-cores + E-cores); then
// sysmon_cpu_core_is_efficient() says which group a thread belongs to.
bool sysmon_cpu_hybrid(void);
bool sysmon_cpu_core_is_efficient(int index);

// Usage % / temperature °C of the last SYSMON_CPU_HISTORY_LEN 5s polls (10
// minutes), for the dropdown's graphs. Index 0 is the oldest.
#define SYSMON_CPU_HISTORY_LEN 120
int sysmon_cpu_history_len(void);
double sysmon_cpu_util_history(int index);
double sysmon_cpu_temp_history(int index);

// Dropdown-only details: per-thread current clock, running/total tasks,
// uptime, and one /proc process scan for the top-processes list (~1-3ms).
// Never called by the 5s poll itself.
void sysmon_cpu_refresh_details(void);
double sysmon_cpu_core_ghz(int index);
int sysmon_cpu_tasks_running(void);
int sysmon_cpu_tasks_total(void);
double sysmon_cpu_uptime_s(void);

// Top CPU users, grouped by process name, as % of the whole CPU (all of
// them together roughly add up to sysmon_cpu_percent()), measured between
// the last two refresh_details() calls. Not ready until two scans since the
// last reset -- call the reset when the dropdown opens, so the first figure
// isn't averaged over however long it was closed.
#define SYSMON_CPU_MAX_PROCS 5
void sysmon_cpu_reset_process_sample(void);
bool sysmon_cpu_process_ready(void);
int sysmon_cpu_process_count(void);
const char *sysmon_cpu_process_name(int index);
double sysmon_cpu_process_percent(int index);

// --- GPU (NVIDIA only, via NVML) ---
// False until the first successful NVML query -- lets the GPU module
// show an explicit "unavailable" state instead of a misleading 0%/N/A mix on
// a machine with no discrete NVIDIA GPU (or a driver that isn't loaded).
bool sysmon_gpu_available(void);
int sysmon_gpu_percent(void);
int sysmon_gpu_mem_percent(void);
int sysmon_gpu_temp_c(void);

// Fetches the dropdown-only GPU fields below (VRAM, fan, power, P-state,
// clocks, encoder/decoder load, the per-app VRAM list) --
// and re-reads usage/temperature, so all of them are one current snapshot.
// They're deliberately *not* read on the 5s poll: the fan query alone costs
// 0.8-5.3ms per call. Call right before showing them; no-op without NVML.
void sysmon_gpu_refresh_details(void);

// Only current as of the last sysmon_gpu_refresh_details() call.
int sysmon_gpu_mem_used_mb(void);
int sysmon_gpu_mem_total_mb(void);
int sysmon_gpu_fan_percent(void);
double sysmon_gpu_power_w(void);
const char *sysmon_gpu_pstate(void); // e.g. "P8", stable buffer
int sysmon_gpu_clock_mhz(void);        // current graphics clock
int sysmon_gpu_mem_clock_mhz(void);    // current memory clock
int sysmon_gpu_encoder_percent(void);  // NVENC load
int sysmon_gpu_decoder_percent(void);  // NVDEC load

// The biggest VRAM users (graphics + compute processes merged by PID),
// largest first, also as of the last refresh_details(). Names are the short
// /proc/PID/comm name.
#define SYSMON_GPU_MAX_PROCS 5
int sysmon_gpu_process_count(void);
const char *sysmon_gpu_process_name(int index);
int sysmon_gpu_process_mib(int index);

// Static, read once at startup ("" / 0 if the driver doesn't report it).
const char *sysmon_gpu_name(void);   // e.g. "NVIDIA GeForce RTX 3080"
const char *sysmon_gpu_driver(void); // e.g. "615.78.08"
double sysmon_gpu_power_limit_w(void);
int sysmon_gpu_clock_max_mhz(void);
int sysmon_gpu_mem_clock_max_mhz(void);

// Usage % / temperature °C of the last SYSMON_GPU_HISTORY_LEN 5s polls (10
// minutes), for the dropdown's graphs. Index 0 is the oldest.
#define SYSMON_GPU_HISTORY_LEN 120
int sysmon_gpu_history_len(void);
double sysmon_gpu_util_history(int index);
double sysmon_gpu_temp_history(int index);

// --- Memory ---
int sysmon_mem_percent(void);
double sysmon_mem_used_gb(void);  // total - available: what apps hold ("apps" in the dropdown)
double sysmon_mem_total_gb(void);
double sysmon_mem_avail_gb(void); // MemAvailable: free + what the kernel can reclaim
double sysmon_mem_free_gb(void);  // MemFree: truly unused, usually small
double sysmon_mem_cache_gb(void); // available - free: reclaimable cache
double sysmon_mem_shared_gb(void);
double sysmon_mem_dirty_mb(void);
double sysmon_swap_used_gb(void);
double sysmon_swap_total_gb(void);
int sysmon_swap_percent(void);

// Used % / swap % of the last SYSMON_MEM_HISTORY_LEN 5s polls (10 minutes).
// Index 0 is the oldest.
#define SYSMON_MEM_HISTORY_LEN 120
int sysmon_mem_history_len(void);
double sysmon_mem_used_history(int index);
double sysmon_mem_swap_history(int index);

// Dropdown-only details: memory pressure (PSI), zram stats and the top apps
// by private memory. Not part of the 5s poll; the dropdown calls this on
// open and on each poll while it's open. A snapshot, so unlike the CPU's
// process list it's ready immediately.
void sysmon_mem_refresh_details(void);

// PSI "some" averages from /proc/pressure/memory: % of time at least one
// task was stalled waiting on memory. window 0/1/2 = 10s/60s/300s.
bool sysmon_mem_psi_available(void);
double sysmon_mem_psi_some(int window);

// Swap backed by a /dev/zramN device (compressed RAM), with its stats from
// /sys/block/zramN/mm_stat. Sizes in MiB.
bool sysmon_swap_is_zram(void);
const char *sysmon_zram_algorithm(void); // e.g. "zstd"
double sysmon_zram_orig_mb(void);        // data stored, uncompressed
double sysmon_zram_compr_mb(void);       // the same data compressed
double sysmon_zram_used_mb(void);        // RAM zram really uses (incl. overhead)

// Top apps by private resident memory (statm resident - shared, i.e. RssAnon),
// summed by process name. Biggest first.
#define SYSMON_MEM_MAX_PROCS 5
int sysmon_mem_process_count(void);
const char *sysmon_mem_process_name(int index);
double sysmon_mem_process_gb(int index);

#endif
