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
void sysmon_client_init(void);

// Re-reads /proc/stat, /proc/meminfo, /sys/class/thermal/thermal_zone1/temp,
// /proc/cpuinfo, /proc/loadavg, and (synchronously, via popen() -- see
// CLAUDE.md's "System monitor (CPU/GPU/memory)" section for why that's an
// acceptable blocking window here) nvidia-smi. Call once at startup and then
// on every tick of a 5-second timerfd that main.c owns and drives directly,
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

// --- GPU (NVIDIA only, via nvidia-smi) ---
// False until the first successful nvidia-smi query -- lets the GPU module
// show an explicit "unavailable" state instead of a misleading 0%/N/A mix on
// a machine with no discrete NVIDIA GPU (or a driver that isn't loaded).
bool sysmon_gpu_available(void);
int sysmon_gpu_percent(void);
int sysmon_gpu_mem_percent(void);
int sysmon_gpu_temp_c(void);
int sysmon_gpu_mem_used_mb(void);
int sysmon_gpu_mem_total_mb(void);
int sysmon_gpu_fan_percent(void);
double sysmon_gpu_power_w(void);
const char *sysmon_gpu_pstate(void); // e.g. "P8", stable buffer

// --- Memory ---
int sysmon_mem_percent(void);
double sysmon_mem_used_gb(void);
double sysmon_mem_total_gb(void);
double sysmon_mem_free_gb(void);
double sysmon_swap_used_gb(void);
double sysmon_swap_total_gb(void);
int sysmon_swap_percent(void);

#endif
