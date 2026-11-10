#include "sysmon/sysmon_client.h"

#include <dirent.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/file.h"
#include "util/history.h"

// Comfortably above any real machine's thread count (this development
// machine has 16); matches this project's existing convention (sni_registry's
// MAX_TRAY_ITEMS, workspaces' MAX_OCCUPIED_IDS) of a bounded array over
// dynamic growth.
#define MAX_CPU_CORES 128

typedef struct {
    unsigned long long idle, total;
    bool has_prev;
} JiffiesSample;

static JiffiesSample g_prev_total;
static JiffiesSample g_prev_cores[MAX_CPU_CORES];

static double g_cpu_percent = 0.0;
static int g_cpu_temp_c = 0;
static double g_cpu_freq_ghz = 0.0;
static char g_load_avg[64] = "0.00 0.00 0.00";
static double g_core_percent[MAX_CPU_CORES];
static int g_core_count = 0;

// Static CPU facts for the dropdown, read once in sysmon_client_init().
static char g_cpu_model[96] = "";
static int g_cpu_physical_cores = 0;
static double g_cpu_max_ghz = 0.0;
// Hybrid Intel chips (12th gen+) list their P-core and E-core threads in
// /sys/devices/cpu_core/cpus and /sys/devices/cpu_atom/cpus; neither exists
// on AMD or older Intel, which then show one plain "CORES" group.
static bool g_cpu_hybrid = false;
static bool g_core_efficient[MAX_CPU_CORES];

// Dropdown-only live values (sysmon_cpu_refresh_details()).
static double g_core_ghz[MAX_CPU_CORES];
static int g_tasks_running = 0, g_tasks_total = 0;
static double g_uptime_s = 0.0;

// Usage/temperature history, same shape as the GPU's below.
static History2 g_cpu_hist; // a = usage %, b = temperature

// Top-processes sample: per-PID utime+stime from the previous /proc scan,
// in an open-addressing table keyed by PID (two tables swapped each scan, so
// PIDs that exited simply drop out), and the resulting top few by name.
#define PROC_TABLE_SIZE 4096 // power of two, well above a desktop's ~400-1500 PIDs
typedef struct {
    int pid; // 0 = empty slot
    unsigned long long ticks;
} ProcTicks;
static ProcTicks g_proc_tables[2][PROC_TABLE_SIZE];
static int g_proc_table_cur = 0;
static bool g_proc_primed = false;     // a previous scan exists to diff against
static bool g_proc_has_result = false; // g_cpu_procs holds a real measurement
static double g_proc_sample_time = 0.0;
typedef struct {
    char name[32];
    double percent;
} CpuProcess;
static CpuProcess g_cpu_procs[SYSMON_CPU_MAX_PROCS];
static int g_cpu_proc_count = 0;

static bool g_gpu_available = false;
static bool g_gpu_warned = false;
static int g_gpu_percent = 0;
static int g_gpu_temp_c = 0;
static int g_gpu_mem_used_mb = 0;
static int g_gpu_mem_total_mb = 0;
static int g_gpu_fan_percent = 0;
static double g_gpu_power_w = 0.0;
static char g_gpu_pstate[16] = "";

// Static facts, read once in nvml_init() ("" / 0 if a query isn't supported).
static char g_gpu_name[96] = "";
static char g_gpu_driver[80] = "";
static double g_gpu_power_limit_w = 0.0;
static int g_gpu_clock_max_mhz = 0;

// Dropdown-only live values, like VRAM/fan/power above.
static int g_gpu_clock_mhz = 0;
static int g_gpu_mem_clock_mhz = 0;
static int g_gpu_encoder_percent = 0;
static int g_gpu_decoder_percent = 0;

typedef struct {
    char name[32]; // /proc/PID/comm (kernel caps it at 15 chars + NUL)
    int mib;
} GpuProcess;
static GpuProcess g_gpu_procs[SYSMON_GPU_MAX_PROCS];
static int g_gpu_proc_count = 0;

// Usage/temperature history for the dropdown's graphs: one sample per 5s
// poll, so SYSMON_GPU_HISTORY_LEN covers 10 minutes (the graph shows as much
// as fits its card). Recorded always (these two reads happen every poll
// anyway, for the bar box), so the graph is full
// the moment the dropdown opens -- same shape as network_client.c's history.
static History2 g_gpu_hist; // a = usage %, b = temperature

static int g_mem_percent = 0;
static double g_mem_used_gb = 0.0;
static double g_mem_total_gb = 0.0;
static double g_mem_avail_gb = 0.0; // MemAvailable (the old dropdown labelled this "FREE")
static double g_mem_free_gb = 0.0;  // MemFree
static double g_mem_shared_gb = 0.0;
static double g_mem_dirty_mb = 0.0;
static double g_swap_used_gb = 0.0;
static double g_swap_total_gb = 0.0;
static int g_swap_percent = 0;

// Used %/swap % history, same shape as the CPU's and GPU's.
static History2 g_mem_hist; // a = used %, b = swap %

// Dropdown-only memory details (sysmon_mem_refresh_details()).
static bool g_psi_available = false;
static double g_psi_some[3];
static bool g_swap_zram = false;
static char g_zram_algorithm[24] = "";
static double g_zram_orig_mb = 0.0, g_zram_compr_mb = 0.0, g_zram_used_mb = 0.0;
typedef struct {
    char name[32];
    double gb;
} MemProcess;
static MemProcess g_mem_procs[SYSMON_MEM_MAX_PROCS];
static int g_mem_proc_count = 0;

static void nvml_init(void);

// Marks every CPU in a kernel cpulist ("0-11", "12-15", "0,2,4-7") as
// efficient (or not). Returns false if the file doesn't exist.
static bool mark_cpulist(const char *path, bool efficient) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }
    char buf[256];
    bool ok = fgets(buf, sizeof(buf), f) != NULL;
    fclose(f);
    if (!ok) {
        return false;
    }
    char *p = buf;
    while (*p && *p != '\n') {
        char *end;
        long lo = strtol(p, &end, 10);
        if (end == p) {
            break;
        }
        long hi = lo;
        p = end;
        if (*p == '-') {
            hi = strtol(p + 1, &end, 10);
            p = end;
        }
        for (long i = lo; i <= hi && i < MAX_CPU_CORES; i++) {
            if (i >= 0) {
                g_core_efficient[i] = efficient;
            }
        }
        if (*p == ',') {
            p++;
        }
    }
    return true;
}

static void read_cpu_static_info(void) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f) && (!g_cpu_model[0] || !g_cpu_physical_cores)) {
            char *colon = strchr(line, ':');
            if (!colon) {
                continue;
            }
            char *value = colon + 1;
            while (*value == ' ') {
                value++;
            }
            value[strcspn(value, "\n")] = '\0';
            if (!g_cpu_model[0] && strncmp(line, "model name", 10) == 0) {
                snprintf(g_cpu_model, sizeof(g_cpu_model), "%s", value);
            } else if (!g_cpu_physical_cores && strncmp(line, "cpu cores", 9) == 0) {
                g_cpu_physical_cores = atoi(value);
            }
        }
        fclose(f);
    }

    long khz = 0;
    if (file_read_long("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", &khz)) {
        g_cpu_max_ghz = khz / 1e6;
    }

    bool has_p = mark_cpulist("/sys/devices/cpu_core/cpus", false);
    bool has_e = mark_cpulist("/sys/devices/cpu_atom/cpus", true);
    g_cpu_hybrid = has_p && has_e;
}

void sysmon_client_init(void) {
    memset(&g_prev_total, 0, sizeof(g_prev_total));
    memset(g_prev_cores, 0, sizeof(g_prev_cores));
    read_cpu_static_info();
    nvml_init();
}

static void poll_cpu(void) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) {
        return;
    }

    char line[512];
    int core_idx = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) != 0) {
            break; // "cpu"/"cpuN" lines are always first and contiguous in /proc/stat
        }

        char label[16];
        unsigned long long fields[10] = { 0 };
        int n = sscanf(line, "%15s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                        label, &fields[0], &fields[1], &fields[2], &fields[3], &fields[4],
                        &fields[5], &fields[6], &fields[7], &fields[8], &fields[9]);
        if (n < 5) {
            continue; // need at least through the idle field
        }

        unsigned long long idle = fields[3];
        unsigned long long total = 0;
        for (int i = 0; i < 10; i++) {
            total += fields[i];
        }

        bool is_aggregate = strcmp(label, "cpu") == 0;
        JiffiesSample *prev = is_aggregate ? &g_prev_total
                                            : (core_idx < MAX_CPU_CORES ? &g_prev_cores[core_idx] : NULL);

        double percent = 0.0;
        if (prev && prev->has_prev) {
            unsigned long long idle_delta = idle - prev->idle;
            unsigned long long total_delta = total - prev->total;
            percent = total_delta > 0 ? (double)(total_delta - idle_delta) * 100.0 / (double)total_delta : 0.0;
        }
        if (prev) {
            prev->idle = idle;
            prev->total = total;
            prev->has_prev = true;
        }

        if (is_aggregate) {
            g_cpu_percent = percent;
        } else if (core_idx < MAX_CPU_CORES) {
            g_core_percent[core_idx] = percent;
            core_idx++;
        }
    }
    fclose(f);
    g_core_count = core_idx;

    long milli_c = 0;
    if (file_read_long("/sys/class/thermal/thermal_zone1/temp", &milli_c)) {
        g_cpu_temp_c = (int)lround((double)milli_c / 1000.0);
    }

    // The load averages and the /proc/cpuinfo clock average are dropdown-only
    // -- see sysmon_cpu_refresh_details(). Reading them here meant parsing
    // the whole 28 KB /proc/cpuinfo every 5s for a fallback value.
}

// The average "cpu MHz" over all cores from /proc/cpuinfo -- only a fallback,
// for when cpufreq's per-core scaling_cur_freq isn't exposed.
static double cpuinfo_avg_ghz(void) {
    FILE *cf = fopen("/proc/cpuinfo", "r");
    if (!cf) {
        return 0.0;
    }
    char line[256];
    double sum = 0.0;
    int count = 0;
    while (fgets(line, sizeof(line), cf)) {
        if (strncmp(line, "cpu MHz", 7) != 0) {
            continue;
        }
        char *colon = strchr(line, ':');
        if (colon) {
            sum += atof(colon + 1);
            count++;
        }
    }
    fclose(cf);
    return count > 0 ? (sum / count) / 1000.0 : 0.0;
}

static long long parse_meminfo_field(const char *text, const char *key) {
    const char *p = strstr(text, key);
    if (!p) {
        return -1;
    }
    p += strlen(key);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return atoll(p);
}

static void poll_mem(void) {
    char buf[8192];
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) {
        return;
    }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    long long mem_total_kb = parse_meminfo_field(buf, "MemTotal:");
    long long mem_avail_kb = parse_meminfo_field(buf, "MemAvailable:");
    if (mem_total_kb <= 0 || mem_avail_kb < 0) {
        return;
    }
    long long mem_used_kb = mem_total_kb - mem_avail_kb;

    g_mem_percent = (int)lround(100.0 * (double)mem_used_kb / (double)mem_total_kb);
    g_mem_used_gb = (double)mem_used_kb / 1024.0 / 1024.0;
    g_mem_total_gb = (double)mem_total_kb / 1024.0 / 1024.0;
    g_mem_avail_gb = (double)mem_avail_kb / 1024.0 / 1024.0;
    // Only the dropdown shows these; a missing field (-1) reads as 0.
    long long free_kb = parse_meminfo_field(buf, "MemFree:");
    long long shmem_kb = parse_meminfo_field(buf, "Shmem:");
    long long dirty_kb = parse_meminfo_field(buf, "Dirty:");
    g_mem_free_gb = free_kb > 0 ? (double)free_kb / 1024.0 / 1024.0 : 0.0;
    g_mem_shared_gb = shmem_kb > 0 ? (double)shmem_kb / 1024.0 / 1024.0 : 0.0;
    g_mem_dirty_mb = dirty_kb > 0 ? (double)dirty_kb / 1024.0 : 0.0;

    long long swap_total_kb = parse_meminfo_field(buf, "SwapTotal:");
    long long swap_free_kb = parse_meminfo_field(buf, "SwapFree:");
    if (swap_total_kb >= 0 && swap_free_kb >= 0) {
        long long swap_used_kb = swap_total_kb - swap_free_kb;
        g_swap_total_gb = (double)swap_total_kb / 1024.0 / 1024.0;
        g_swap_used_gb = (double)swap_used_kb / 1024.0 / 1024.0;
        g_swap_percent = swap_total_kb > 0 ? (int)lround(100.0 * (double)swap_used_kb / (double)swap_total_kb) : 0;
    }
}

/*
 * GPU stats come from NVML (libnvidia-ml), the library nvidia-smi itself is a
 * thin CLI over -- calling it directly replaces a fork+exec of nvidia-smi every
 * 5 seconds (~15-45ms of blocked event loop each time) with a handful of
 * in-process calls taking microseconds.
 *
 * Loaded with dlopen() at runtime rather than linked:
 *   - nvml.h only ships with the CUDA toolkit (/opt/cuda here), not with the
 *     driver, so linking would add CUDA as a build dependency just for nine
 *     function prototypes;
 *   - linking against libnvidia-ml.so would make stickBar fail to *start* on a
 *     machine without the NVIDIA driver, instead of just showing "--" in the
 *     GPU box like it always has.
 * The library itself comes from nvidia-utils -- the same package that ships
 * nvidia-smi -- so "is GPU monitoring available" hasn't changed at all.
 *
 * The few types/prototypes needed are declared locally below, copied from
 * nvml.h. NVML keeps these versioned entry points ABI-stable (that's exactly
 * what the _v2 suffixes are for), so this doesn't drift with driver updates.
 */
typedef int nvmlReturn_t; // NVML_SUCCESS == 0, anything else is an error
typedef void *nvmlDevice_t; // opaque handle
typedef struct {
    unsigned int gpu;    // % of the last sample period a kernel was running
    unsigned int memory; // % of the last sample period memory was read/written
} nvmlUtilization_t;
// The _v2 memory query, not the original: v1's `used` also counts memory the
// driver/firmware reserves for itself (~300 MiB more on this machine), while
// nvidia-smi's own memory.used -- and therefore what this box always showed --
// is v2's, which excludes it. Confirmed side by side against nvidia-smi.
typedef struct {
    unsigned int version; // must be set to NVML_MEMORY_V2 before the call
    unsigned long long total, reserved, free, used; // bytes
} nvmlMemory_v2_t;
#define NVML_MEMORY_V2 ((unsigned int)(sizeof(nvmlMemory_v2_t) | (2u << 24)))
#define NVML_TEMPERATURE_GPU 0
#define NVML_CLOCK_GRAPHICS 0
#define NVML_CLOCK_MEM 2
// nvmlProcessInfo_v2_t, which is what the _v3 running-process queries fill
// in (checked against /opt/cuda's nvml.h). usedGpuMemory is bytes, or
// NVML_VALUE_NOT_AVAILABLE (all bits set) when the driver can't say.
typedef struct {
    unsigned int pid;
    unsigned long long usedGpuMemory;
    unsigned int gpuInstanceId, computeInstanceId;
} nvmlProcessInfo_t;
#define NVML_VALUE_NOT_AVAILABLE_ULL (~0ULL)
#define MAX_NVML_PROCS 64 // per query; far above any real desktop's GPU client count

static struct {
    void *lib; // dlopen() handle, NULL if NVML isn't usable this run
    nvmlDevice_t device;
    nvmlReturn_t (*shutdown)(void);
    nvmlReturn_t (*get_utilization)(nvmlDevice_t, nvmlUtilization_t *);
    nvmlReturn_t (*get_temperature)(nvmlDevice_t, int, unsigned int *);
    nvmlReturn_t (*get_memory)(nvmlDevice_t, nvmlMemory_v2_t *);
    nvmlReturn_t (*get_fan)(nvmlDevice_t, unsigned int *);
    nvmlReturn_t (*get_power)(nvmlDevice_t, unsigned int *);   // milliwatts
    nvmlReturn_t (*get_pstate)(nvmlDevice_t, int *);           // 0..15 => "P0".."P15"

    // Optional: only used by the redesigned dropdown. A driver missing any of
    // these just leaves that field empty -- it never disables the GPU module.
    nvmlReturn_t (*get_clock)(nvmlDevice_t, int, unsigned int *);          // MHz
    nvmlReturn_t (*get_encoder)(nvmlDevice_t, unsigned int *, unsigned int *); // %, sampling period
    nvmlReturn_t (*get_decoder)(nvmlDevice_t, unsigned int *, unsigned int *);
    nvmlReturn_t (*get_graphics_procs)(nvmlDevice_t, unsigned int *, nvmlProcessInfo_t *);
    nvmlReturn_t (*get_compute_procs)(nvmlDevice_t, unsigned int *, nvmlProcessInfo_t *);
} g_nvml;

static void gpu_warn_once(const char *why) {
    if (!g_gpu_warned) {
        fprintf(stderr, "stickBar: %s -- GPU module will show \"unavailable\" for this run\n", why);
        g_gpu_warned = true;
    }
}

// One-time setup: load the library, init NVML, resolve every function, and
// grab GPU 0's handle (same "first GPU only" scope nvidia-smi's own single
// CSV line had here). Any failure leaves g_nvml.lib NULL, which poll_gpu()
// treats as "no GPU" -- identical to the old missing-nvidia-smi behavior.
static void nvml_init(void) {
    void *lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        gpu_warn_once("NVIDIA's libnvidia-ml.so.1 not found (no NVIDIA driver?)");
        return;
    }

    nvmlReturn_t (*init)(void) = (nvmlReturn_t (*)(void))dlsym(lib, "nvmlInit_v2");
    nvmlReturn_t (*get_handle)(unsigned int, nvmlDevice_t *) =
        (nvmlReturn_t (*)(unsigned int, nvmlDevice_t *))dlsym(lib, "nvmlDeviceGetHandleByIndex_v2");
    g_nvml.shutdown = (nvmlReturn_t (*)(void))dlsym(lib, "nvmlShutdown");
    g_nvml.get_utilization = (nvmlReturn_t (*)(nvmlDevice_t, nvmlUtilization_t *))dlsym(lib, "nvmlDeviceGetUtilizationRates");
    g_nvml.get_temperature = (nvmlReturn_t (*)(nvmlDevice_t, int, unsigned int *))dlsym(lib, "nvmlDeviceGetTemperature");
    g_nvml.get_memory = (nvmlReturn_t (*)(nvmlDevice_t, nvmlMemory_v2_t *))dlsym(lib, "nvmlDeviceGetMemoryInfo_v2");
    g_nvml.get_fan = (nvmlReturn_t (*)(nvmlDevice_t, unsigned int *))dlsym(lib, "nvmlDeviceGetFanSpeed");
    g_nvml.get_power = (nvmlReturn_t (*)(nvmlDevice_t, unsigned int *))dlsym(lib, "nvmlDeviceGetPowerUsage");
    g_nvml.get_pstate = (nvmlReturn_t (*)(nvmlDevice_t, int *))dlsym(lib, "nvmlDeviceGetPerformanceState");

    if (!init || !get_handle || !g_nvml.shutdown || !g_nvml.get_utilization || !g_nvml.get_temperature ||
        !g_nvml.get_memory || !g_nvml.get_fan || !g_nvml.get_power || !g_nvml.get_pstate) {
        gpu_warn_once("libnvidia-ml.so.1 is missing an expected NVML function");
        dlclose(lib);
        return;
    }
    if (init() != 0) {
        gpu_warn_once("nvmlInit failed (NVIDIA driver not loaded?)");
        dlclose(lib);
        return;
    }
    if (get_handle(0, &g_nvml.device) != 0) {
        gpu_warn_once("NVML found no GPU at index 0");
        g_nvml.shutdown();
        dlclose(lib);
        return;
    }
    g_nvml.lib = lib;

    // Optional dropdown-only symbols (see g_nvml above).
    g_nvml.get_clock = (nvmlReturn_t (*)(nvmlDevice_t, int, unsigned int *))dlsym(lib, "nvmlDeviceGetClockInfo");
    g_nvml.get_encoder = (nvmlReturn_t (*)(nvmlDevice_t, unsigned int *, unsigned int *))dlsym(lib, "nvmlDeviceGetEncoderUtilization");
    g_nvml.get_decoder = (nvmlReturn_t (*)(nvmlDevice_t, unsigned int *, unsigned int *))dlsym(lib, "nvmlDeviceGetDecoderUtilization");
    g_nvml.get_graphics_procs = (nvmlReturn_t (*)(nvmlDevice_t, unsigned int *, nvmlProcessInfo_t *))dlsym(lib, "nvmlDeviceGetGraphicsRunningProcesses_v3");
    g_nvml.get_compute_procs = (nvmlReturn_t (*)(nvmlDevice_t, unsigned int *, nvmlProcessInfo_t *))dlsym(lib, "nvmlDeviceGetComputeRunningProcesses_v3");

    // Static facts, read once: they don't change while the driver is loaded.
    nvmlReturn_t (*get_name)(nvmlDevice_t, char *, unsigned int) =
        (nvmlReturn_t (*)(nvmlDevice_t, char *, unsigned int))dlsym(lib, "nvmlDeviceGetName");
    nvmlReturn_t (*get_driver)(char *, unsigned int) = (nvmlReturn_t (*)(char *, unsigned int))dlsym(lib, "nvmlSystemGetDriverVersion");
    nvmlReturn_t (*get_power_limit)(nvmlDevice_t, unsigned int *) =
        (nvmlReturn_t (*)(nvmlDevice_t, unsigned int *))dlsym(lib, "nvmlDeviceGetEnforcedPowerLimit");
    nvmlReturn_t (*get_max_clock)(nvmlDevice_t, int, unsigned int *) =
        (nvmlReturn_t (*)(nvmlDevice_t, int, unsigned int *))dlsym(lib, "nvmlDeviceGetMaxClockInfo");
    if (get_name && get_name(g_nvml.device, g_gpu_name, sizeof(g_gpu_name)) != 0) {
        g_gpu_name[0] = '\0';
    }
    if (get_driver && get_driver(g_gpu_driver, sizeof(g_gpu_driver)) != 0) {
        g_gpu_driver[0] = '\0';
    }
    unsigned int value = 0;
    if (get_power_limit && get_power_limit(g_nvml.device, &value) == 0) {
        g_gpu_power_limit_w = value / 1000.0; // milliwatts
    }
    if (get_max_clock && get_max_clock(g_nvml.device, NVML_CLOCK_GRAPHICS, &value) == 0) {
        g_gpu_clock_max_mhz = (int)value;
    }
}


// Merges one NVML running-process list into `merged` by PID. An app doing
// both graphics and compute ("C+G" in nvidia-smi) appears in both lists with
// the same figure, so keep the larger rather than adding them.
static void merge_procs(nvmlReturn_t (*query)(nvmlDevice_t, unsigned int *, nvmlProcessInfo_t *),
                        nvmlProcessInfo_t *merged, unsigned int *merged_count) {
    if (!query) {
        return;
    }
    nvmlProcessInfo_t list[MAX_NVML_PROCS];
    unsigned int count = MAX_NVML_PROCS;
    // A list longer than the buffer fails with INSUFFICIENT_SIZE and fills
    // nothing; at 64 that would take a very unusual machine, so just skip.
    if (query(g_nvml.device, &count, list) != 0) {
        return;
    }
    for (unsigned int i = 0; i < count && i < MAX_NVML_PROCS; i++) {
        unsigned long long used = list[i].usedGpuMemory == NVML_VALUE_NOT_AVAILABLE_ULL ? 0 : list[i].usedGpuMemory;
        unsigned int j = 0;
        while (j < *merged_count && merged[j].pid != list[i].pid) {
            j++;
        }
        if (j < *merged_count) {
            if (used > merged[j].usedGpuMemory) {
                merged[j].usedGpuMemory = used;
            }
        } else if (*merged_count < MAX_NVML_PROCS) {
            merged[*merged_count] = list[i];
            merged[*merged_count].usedGpuMemory = used;
            (*merged_count)++;
        }
    }
}

// Rebuilds g_gpu_procs: the SYSMON_GPU_MAX_PROCS biggest VRAM users, named
// from /proc/PID/comm -- the short process name, so a Chromium-based
// browser's GPU process reads "helium" instead of nvidia-smi's multi-line
// command line.
static void refresh_gpu_procs(void) {
    nvmlProcessInfo_t merged[MAX_NVML_PROCS];
    unsigned int n = 0;
    merge_procs(g_nvml.get_graphics_procs, merged, &n);
    merge_procs(g_nvml.get_compute_procs, merged, &n);

    // Selection of the top few by memory -- n is small, so a simple
    // repeated-max pass is plenty.
    g_gpu_proc_count = 0;
    while (g_gpu_proc_count < SYSMON_GPU_MAX_PROCS) {
        int best = -1;
        for (unsigned int i = 0; i < n; i++) {
            if (merged[i].pid != 0 && (best < 0 || merged[i].usedGpuMemory > merged[best].usedGpuMemory)) {
                best = (int)i;
            }
        }
        if (best < 0) {
            break;
        }
        GpuProcess *p = &g_gpu_procs[g_gpu_proc_count++];
        p->mib = (int)(merged[best].usedGpuMemory / (1024 * 1024));
        char path[64];
        snprintf(path, sizeof(path), "/proc/%u/comm", merged[best].pid);
        FILE *f = fopen(path, "r");
        if (!f || !fgets(p->name, sizeof(p->name), f)) {
            snprintf(p->name, sizeof(p->name), "pid %u", merged[best].pid);
        }
        if (f) {
            fclose(f);
        }
        p->name[strcspn(p->name, "\n")] = '\0';
        merged[best].pid = 0; // taken
    }
}

static void poll_gpu(void) {
    if (!g_nvml.lib) {
        return;
    }

    // Only what the bar box itself shows (usage + temperature) is read on the
    // 5s poll; the rest lives only in the dropdown and is fetched when it opens
    // (sysmon_gpu_refresh_details()). Measured in-bar, the fan query alone took
    // 0.8-5.3ms per call -- ~90% of the whole poll -- while these two are ~0.01ms.
    //
    // Utilization is the one field the GPU box can't do without -- if even that
    // fails (e.g. the GPU fell off the bus), keep the previous values, the same
    // "leave old values in place" behavior a malformed nvidia-smi line had.
    nvmlUtilization_t util;
    if (g_nvml.get_utilization(g_nvml.device, &util) != 0) {
        return;
    }
    g_gpu_percent = (int)util.gpu;

    unsigned int value = 0;
    g_gpu_temp_c = g_nvml.get_temperature(g_nvml.device, NVML_TEMPERATURE_GPU, &value) == 0 ? (int)value : 0;

    g_gpu_available = true;
}

void sysmon_gpu_refresh_details(void) {
    if (!g_nvml.lib) {
        return;
    }

    // Re-read usage/temperature too, so everything in the dropdown is from the
    // same moment rather than mixing fresh details with a poll up to 5s old.
    poll_gpu();

    // Best-effort per field: a card without a fan (or a driver refusing one
    // query) shouldn't blank the whole dropdown.
    unsigned int value = 0;
    g_gpu_fan_percent = g_nvml.get_fan(g_nvml.device, &value) == 0 ? (int)value : 0;
    g_gpu_power_w = g_nvml.get_power(g_nvml.device, &value) == 0 ? value / 1000.0 : 0.0;

    nvmlMemory_v2_t mem = { .version = NVML_MEMORY_V2 };
    if (g_nvml.get_memory(g_nvml.device, &mem) == 0) {
        // MiB, matching nvidia-smi's memory.used/memory.total units.
        g_gpu_mem_used_mb = (int)(mem.used / (1024 * 1024));
        g_gpu_mem_total_mb = (int)(mem.total / (1024 * 1024));
    }

    int pstate = 0;
    if (g_nvml.get_pstate(g_nvml.device, &pstate) == 0 && pstate >= 0 && pstate <= 15) {
        snprintf(g_gpu_pstate, sizeof(g_gpu_pstate), "P%d", pstate);
    } else {
        g_gpu_pstate[0] = '\0';
    }

    unsigned int period = 0;
    g_gpu_clock_mhz = g_nvml.get_clock && g_nvml.get_clock(g_nvml.device, NVML_CLOCK_GRAPHICS, &value) == 0 ? (int)value : 0;
    g_gpu_mem_clock_mhz = g_nvml.get_clock && g_nvml.get_clock(g_nvml.device, NVML_CLOCK_MEM, &value) == 0 ? (int)value : 0;
    g_gpu_encoder_percent = g_nvml.get_encoder && g_nvml.get_encoder(g_nvml.device, &value, &period) == 0 ? (int)value : 0;
    g_gpu_decoder_percent = g_nvml.get_decoder && g_nvml.get_decoder(g_nvml.device, &value, &period) == 0 ? (int)value : 0;
    refresh_gpu_procs();
}

static ProcTicks *proc_slot(ProcTicks *table, int pid) {
    unsigned int h = ((unsigned int)pid * 2654435761u) & (PROC_TABLE_SIZE - 1);
    while (table[h].pid != 0 && table[h].pid != pid) {
        h = (h + 1) & (PROC_TABLE_SIZE - 1);
    }
    return &table[h];
}

// Reads /proc/PID/stat into `buf` and copies the process name into `name`.
// Returns a pointer just past the name's closing ")" (the state field comes
// next), or NULL if the process is gone or the line is malformed.
static char *read_proc_stat(int pid, char *buf, size_t buf_size, char name[32]) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL; // exited since readdir()
    }
    size_t n = fread(buf, 1, buf_size - 1, f);
    fclose(f);
    buf[n] = '\0';

    // "PID (comm) S ppid ...": comm may itself contain spaces or ')',
    // so it runs from the first '(' to the *last* ')'.
    char *open_paren = strchr(buf, '(');
    char *close_paren = strrchr(buf, ')');
    if (!open_paren || !close_paren || close_paren < open_paren) {
        return NULL;
    }
    size_t len = (size_t)(close_paren - open_paren - 1);
    if (len >= 32) {
        len = 31;
    }
    memcpy(name, open_paren + 1, len);
    name[len] = '\0';
    return close_paren;
}

// One pass over /proc/[0-9]*/stat: records every process's utime+stime and,
// if a previous pass exists, turns the per-PID differences into "% of the
// whole CPU" summed by process name, keeping the top SYSMON_CPU_MAX_PROCS.
// ~1-3ms for this desktop's ~400 processes; only the dropdown calls it.
static void sample_cpu_processes(void) {
    DIR *dir = opendir("/proc");
    if (!dir) {
        return;
    }
    double now = monotonic_seconds();
    ProcTicks *prev = g_proc_tables[g_proc_table_cur];
    ProcTicks *cur = g_proc_tables[1 - g_proc_table_cur];
    memset(cur, 0, sizeof(g_proc_tables[0]));

    // Grouped by name: a browser's renderer/GPU/utility processes add up to
    // one line, which says more about "what's using my CPU" than 20 lines.
    enum { MAX_NAMES = 512 };
    static CpuProcess groups[MAX_NAMES];
    int group_count = 0;
    double denom = (now - g_proc_sample_time) * (double)sysconf(_SC_CLK_TCK) * (g_core_count > 0 ? g_core_count : 1);

    struct dirent *ent;
    int stored = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') {
            continue;
        }
        int pid = atoi(ent->d_name);
        char buf[512], name[32];
        char *close_paren = read_proc_stat(pid, buf, sizeof(buf), name);
        if (!close_paren) {
            continue;
        }
        // After ") " come fields 3.. (state first); utime/stime are fields
        // 14 and 15, i.e. the 12th and 13th after the state.
        unsigned long long utime = 0, stime = 0;
        if (sscanf(close_paren + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime, &stime) != 2) {
            continue;
        }
        unsigned long long ticks = utime + stime;
        if (stored < PROC_TABLE_SIZE / 2) { // keep the table at most half full
            ProcTicks *slot = proc_slot(cur, pid);
            slot->pid = pid;
            slot->ticks = ticks;
            stored++;
        }

        if (!g_proc_primed || denom <= 0.0) {
            continue;
        }
        ProcTicks *old = proc_slot(prev, pid);
        if (old->pid != pid || ticks <= old->ticks) {
            continue; // new since the last scan, or idle
        }
        double percent = (double)(ticks - old->ticks) * 100.0 / denom;
        int g = 0;
        while (g < group_count && strcmp(groups[g].name, name) != 0) {
            g++;
        }
        if (g == group_count) {
            if (group_count == MAX_NAMES) {
                continue;
            }
            snprintf(groups[g].name, sizeof(groups[g].name), "%s", name);
            groups[g].percent = 0.0;
            group_count++;
        }
        groups[g].percent += percent;
    }
    closedir(dir);

    if (g_proc_primed) {
        g_cpu_proc_count = 0;
        while (g_cpu_proc_count < SYSMON_CPU_MAX_PROCS) {
            int best = -1;
            for (int i = 0; i < group_count; i++) {
                if (groups[i].percent > 0.0 && (best < 0 || groups[i].percent > groups[best].percent)) {
                    best = i;
                }
            }
            if (best < 0) {
                break;
            }
            g_cpu_procs[g_cpu_proc_count++] = groups[best];
            groups[best].percent = 0.0; // taken
        }
        g_proc_has_result = true;
    }
    g_proc_table_cur = 1 - g_proc_table_cur;
    g_proc_sample_time = now;
    g_proc_primed = true;
}

void sysmon_cpu_refresh_details(void) {
    bool have_core_freqs = false;
    for (int i = 0; i < g_core_count; i++) {
        char path[96];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
        long khz = 0;
        g_core_ghz[i] = file_read_long(path, &khz) ? khz / 1e6 : 0.0;
        have_core_freqs |= g_core_ghz[i] > 0.0;
    }
    g_cpu_freq_ghz = have_core_freqs ? 0.0 : cpuinfo_avg_ghz();

    // /proc/loadavg: the 1/5/15-minute averages, then "running/total"
    // scheduling entities.
    FILE *f = fopen("/proc/loadavg", "r");
    if (f) {
        double a = 0, b = 0, c = 0;
        if (fscanf(f, "%lf %lf %lf %d/%d", &a, &b, &c, &g_tasks_running, &g_tasks_total) == 5) {
            snprintf(g_load_avg, sizeof(g_load_avg), "%.2f %.2f %.2f", a, b, c);
        } else {
            g_tasks_running = g_tasks_total = 0;
        }
        fclose(f);
    }
    f = fopen("/proc/uptime", "r");
    if (f) {
        if (fscanf(f, "%lf", &g_uptime_s) != 1) {
            g_uptime_s = 0.0;
        }
        fclose(f);
    }
    sample_cpu_processes();
}

void sysmon_cpu_reset_process_sample(void) {
    g_proc_primed = false;
    g_proc_has_result = false;
    g_cpu_proc_count = 0;
}

// /proc/pressure/memory: "some avg10=0.00 avg60=0.00 avg300=0.00 total=N".
// Absent on kernels built without PSI (CONFIG_PSI) or booted with psi=0.
static void read_memory_pressure(void) {
    FILE *f = fopen("/proc/pressure/memory", "r");
    g_psi_available = f && fscanf(f, "some avg10=%lf avg60=%lf avg300=%lf", &g_psi_some[0], &g_psi_some[1],
                                  &g_psi_some[2]) == 3;
    if (f) {
        fclose(f);
    }
}

// Finds a /dev/zramN in /proc/swaps and reads that device's mm_stat (first
// three fields: orig_data_size, compr_data_size, mem_used_total, in bytes)
// and comp_algorithm (every supported one, the active one in [brackets]).
static void read_zram(void) {
    g_swap_zram = false;
    FILE *f = fopen("/proc/swaps", "r");
    if (!f) {
        return;
    }
    char line[256], dev[32] = "";
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "/dev/%31[a-z0-9]", dev) == 1 && strncmp(dev, "zram", 4) == 0) {
            break;
        }
        dev[0] = '\0';
    }
    fclose(f);
    if (!dev[0]) {
        return;
    }

    char path[96];
    snprintf(path, sizeof(path), "/sys/block/%s/mm_stat", dev);
    f = fopen(path, "r");
    unsigned long long orig = 0, compr = 0, used = 0;
    bool ok = f && fscanf(f, "%llu %llu %llu", &orig, &compr, &used) == 3;
    if (f) {
        fclose(f);
    }
    if (!ok) {
        return;
    }
    g_swap_zram = true;
    g_zram_orig_mb = orig / 1048576.0;
    g_zram_compr_mb = compr / 1048576.0;
    g_zram_used_mb = used / 1048576.0;

    g_zram_algorithm[0] = '\0';
    snprintf(path, sizeof(path), "/sys/block/%s/comp_algorithm", dev);
    f = fopen(path, "r");
    if (f) {
        char algos[256];
        if (fgets(algos, sizeof(algos), f)) {
            char *open = strchr(algos, '[');
            char *close = open ? strchr(open, ']') : NULL;
            if (open && close) {
                *close = '\0';
                snprintf(g_zram_algorithm, sizeof(g_zram_algorithm), "%s", open + 1);
            }
        }
        fclose(f);
    }
}

// One pass over /proc/[0-9]*: each process's private resident memory,
// summed by name, keeping the top SYSMON_MEM_MAX_PROCS.
//
// "Private" is /proc/PID/statm's resident minus shared pages, which equals
// RssAnon. Plain RSS counts every shared library again per process (summed
// for a browser's ~20 processes it said 7.0 GB against a PSS of 3.9 GB);
// PSS itself (smaps_rollup) would be exact but took ~215 ms here and isn't
// readable for other users' processes. Private memory gave 3.5 GB in ~6 ms
// and is readable for every process. A snapshot, no previous scan needed.
static void sample_mem_processes(void) {
    DIR *dir = opendir("/proc");
    if (!dir) {
        return;
    }
    double page_gb = (double)sysconf(_SC_PAGESIZE) / (1024.0 * 1024.0 * 1024.0);
    enum { MAX_NAMES = 512 };
    static MemProcess groups[MAX_NAMES];
    int group_count = 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') {
            continue;
        }
        int pid = atoi(ent->d_name);
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/statm", pid);
        FILE *f = fopen(path, "r");
        if (!f) {
            continue;
        }
        unsigned long long size = 0, resident = 0, shared = 0;
        bool ok = fscanf(f, "%llu %llu %llu", &size, &resident, &shared) == 3;
        fclose(f);
        if (!ok || resident <= shared) {
            continue; // kernel threads have no memory of their own
        }
        char buf[512], name[32];
        if (!read_proc_stat(pid, buf, sizeof(buf), name)) {
            continue;
        }
        int g = 0;
        while (g < group_count && strcmp(groups[g].name, name) != 0) {
            g++;
        }
        if (g == group_count) {
            if (group_count == MAX_NAMES) {
                continue;
            }
            snprintf(groups[g].name, sizeof(groups[g].name), "%s", name);
            groups[g].gb = 0.0;
            group_count++;
        }
        groups[g].gb += (double)(resident - shared) * page_gb;
    }
    closedir(dir);

    g_mem_proc_count = 0;
    while (g_mem_proc_count < SYSMON_MEM_MAX_PROCS) {
        int best = -1;
        for (int i = 0; i < group_count; i++) {
            if (groups[i].gb > 0.0 && (best < 0 || groups[i].gb > groups[best].gb)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        g_mem_procs[g_mem_proc_count++] = groups[best];
        groups[best].gb = 0.0; // taken
    }
}

void sysmon_mem_refresh_details(void) {
    // Fresh totals too, so the dropdown's numbers are from one moment (same
    // reasoning as sysmon_gpu_refresh_details()). History isn't touched.
    poll_mem();
    read_memory_pressure();
    read_zram();
    sample_mem_processes();
}

void sysmon_client_poll(void) {
    poll_cpu();
    poll_mem();
    poll_gpu();
    history2_push(&g_cpu_hist, g_cpu_percent, g_cpu_temp_c);
    // Pushed here rather than inside poll_gpu(), which the dropdown's
    // refresh also calls -- so samples stay exactly one per 5s poll.
    if (g_gpu_available) {
        history2_push(&g_gpu_hist, g_gpu_percent, g_gpu_temp_c);
    }
    history2_push(&g_mem_hist, g_mem_percent, g_swap_percent);
}

void sysmon_client_shutdown(void) {
    if (g_nvml.lib) {
        g_nvml.shutdown();
        dlclose(g_nvml.lib);
        g_nvml.lib = NULL;
    }
}

double sysmon_cpu_percent(void) { return g_cpu_percent; }
int sysmon_cpu_temp_c(void) { return g_cpu_temp_c; }
double sysmon_cpu_freq_ghz(void) { return g_cpu_freq_ghz; }
const char *sysmon_cpu_load_avg(void) { return g_load_avg; }
int sysmon_cpu_core_count(void) { return g_core_count; }
double sysmon_cpu_core_percent(int index) {
    return (index >= 0 && index < g_core_count) ? g_core_percent[index] : 0.0;
}

const char *sysmon_cpu_model(void) { return g_cpu_model; }
int sysmon_cpu_physical_cores(void) { return g_cpu_physical_cores; }
double sysmon_cpu_max_ghz(void) { return g_cpu_max_ghz; }
bool sysmon_cpu_hybrid(void) { return g_cpu_hybrid; }
bool sysmon_cpu_core_is_efficient(int index) {
    return index >= 0 && index < MAX_CPU_CORES && g_core_efficient[index];
}
double sysmon_cpu_core_ghz(int index) {
    return (index >= 0 && index < g_core_count) ? g_core_ghz[index] : 0.0;
}
int sysmon_cpu_tasks_running(void) { return g_tasks_running; }
int sysmon_cpu_tasks_total(void) { return g_tasks_total; }
double sysmon_cpu_uptime_s(void) { return g_uptime_s; }
int sysmon_cpu_history_len(void) { return g_cpu_hist.len; }
double sysmon_cpu_util_history(int index) { return history2_a(&g_cpu_hist, index); }
double sysmon_cpu_temp_history(int index) { return history2_b(&g_cpu_hist, index); }
// Results need two scans (a difference); the first one after a reset only primes.
bool sysmon_cpu_process_ready(void) { return g_proc_has_result; }
int sysmon_cpu_process_count(void) { return g_cpu_proc_count; }
const char *sysmon_cpu_process_name(int index) {
    return (index >= 0 && index < g_cpu_proc_count) ? g_cpu_procs[index].name : "";
}
double sysmon_cpu_process_percent(int index) {
    return (index >= 0 && index < g_cpu_proc_count) ? g_cpu_procs[index].percent : 0.0;
}

bool sysmon_gpu_available(void) { return g_gpu_available; }
int sysmon_gpu_percent(void) { return g_gpu_percent; }
int sysmon_gpu_temp_c(void) { return g_gpu_temp_c; }
int sysmon_gpu_mem_used_mb(void) { return g_gpu_mem_used_mb; }
int sysmon_gpu_mem_total_mb(void) { return g_gpu_mem_total_mb; }
int sysmon_gpu_fan_percent(void) { return g_gpu_fan_percent; }
double sysmon_gpu_power_w(void) { return g_gpu_power_w; }
const char *sysmon_gpu_pstate(void) { return g_gpu_pstate; }
const char *sysmon_gpu_name(void) { return g_gpu_name; }
const char *sysmon_gpu_driver(void) { return g_gpu_driver; }
double sysmon_gpu_power_limit_w(void) { return g_gpu_power_limit_w; }
int sysmon_gpu_clock_mhz(void) { return g_gpu_clock_mhz; }
int sysmon_gpu_clock_max_mhz(void) { return g_gpu_clock_max_mhz; }
int sysmon_gpu_mem_clock_mhz(void) { return g_gpu_mem_clock_mhz; }
int sysmon_gpu_encoder_percent(void) { return g_gpu_encoder_percent; }
int sysmon_gpu_decoder_percent(void) { return g_gpu_decoder_percent; }

int sysmon_gpu_process_count(void) { return g_gpu_proc_count; }
const char *sysmon_gpu_process_name(int index) {
    return (index >= 0 && index < g_gpu_proc_count) ? g_gpu_procs[index].name : "";
}
int sysmon_gpu_process_mib(int index) {
    return (index >= 0 && index < g_gpu_proc_count) ? g_gpu_procs[index].mib : 0;
}

int sysmon_gpu_history_len(void) { return g_gpu_hist.len; }
double sysmon_gpu_util_history(int index) { return history2_a(&g_gpu_hist, index); }
double sysmon_gpu_temp_history(int index) { return history2_b(&g_gpu_hist, index); }

int sysmon_mem_percent(void) { return g_mem_percent; }
double sysmon_mem_used_gb(void) { return g_mem_used_gb; }
double sysmon_mem_total_gb(void) { return g_mem_total_gb; }
double sysmon_mem_avail_gb(void) { return g_mem_avail_gb; }
double sysmon_mem_free_gb(void) { return g_mem_free_gb; }
double sysmon_mem_cache_gb(void) {
    double cache = g_mem_avail_gb - g_mem_free_gb;
    return cache > 0.0 ? cache : 0.0;
}
double sysmon_mem_shared_gb(void) { return g_mem_shared_gb; }
double sysmon_mem_dirty_mb(void) { return g_mem_dirty_mb; }
double sysmon_swap_used_gb(void) { return g_swap_used_gb; }
double sysmon_swap_total_gb(void) { return g_swap_total_gb; }
int sysmon_swap_percent(void) { return g_swap_percent; }

int sysmon_mem_history_len(void) { return g_mem_hist.len; }
double sysmon_mem_used_history(int index) { return history2_a(&g_mem_hist, index); }
double sysmon_mem_swap_history(int index) { return history2_b(&g_mem_hist, index); }

bool sysmon_mem_psi_available(void) { return g_psi_available; }
double sysmon_mem_psi_some(int window) { return (window >= 0 && window < 3) ? g_psi_some[window] : 0.0; }
bool sysmon_swap_is_zram(void) { return g_swap_zram; }
const char *sysmon_zram_algorithm(void) { return g_zram_algorithm; }
double sysmon_zram_orig_mb(void) { return g_zram_orig_mb; }
double sysmon_zram_compr_mb(void) { return g_zram_compr_mb; }
double sysmon_zram_used_mb(void) { return g_zram_used_mb; }

int sysmon_mem_process_count(void) { return g_mem_proc_count; }
const char *sysmon_mem_process_name(int index) {
    return (index >= 0 && index < g_mem_proc_count) ? g_mem_procs[index].name : "";
}
double sysmon_mem_process_gb(int index) {
    return (index >= 0 && index < g_mem_proc_count) ? g_mem_procs[index].gb : 0.0;
}
