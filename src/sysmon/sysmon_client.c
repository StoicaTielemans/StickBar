#include "sysmon/sysmon_client.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static bool g_gpu_available = false;
static bool g_gpu_warned = false;
static int g_gpu_percent = 0;
static int g_gpu_mem_percent = 0;
static int g_gpu_temp_c = 0;
static int g_gpu_mem_used_mb = 0;
static int g_gpu_mem_total_mb = 0;
static int g_gpu_fan_percent = 0;
static double g_gpu_power_w = 0.0;
static char g_gpu_pstate[16] = "";

static int g_mem_percent = 0;
static double g_mem_used_gb = 0.0;
static double g_mem_total_gb = 0.0;
static double g_mem_free_gb = 0.0;
static double g_swap_used_gb = 0.0;
static double g_swap_total_gb = 0.0;
static int g_swap_percent = 0;

static void nvml_init(void);

void sysmon_client_init(void) {
    memset(&g_prev_total, 0, sizeof(g_prev_total));
    memset(g_prev_cores, 0, sizeof(g_prev_cores));
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

    FILE *tf = fopen("/sys/class/thermal/thermal_zone1/temp", "r");
    if (tf) {
        long milli_c = 0;
        if (fscanf(tf, "%ld", &milli_c) == 1) {
            g_cpu_temp_c = (int)lround((double)milli_c / 1000.0);
        }
        fclose(tf);
    }

    FILE *cf = fopen("/proc/cpuinfo", "r");
    if (cf) {
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
        g_cpu_freq_ghz = count > 0 ? (sum / count) / 1000.0 : 0.0;
    }

    FILE *lf = fopen("/proc/loadavg", "r");
    if (lf) {
        double a = 0, b = 0, c = 0;
        if (fscanf(lf, "%lf %lf %lf", &a, &b, &c) == 3) {
            snprintf(g_load_avg, sizeof(g_load_avg), "%.2f %.2f %.2f", a, b, c);
        }
        fclose(lf);
    }
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
    g_mem_free_gb = (double)mem_avail_kb / 1024.0 / 1024.0;

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
}

static void poll_gpu(void) {
    if (!g_nvml.lib) {
        return;
    }

    // Utilization is the one field the GPU box can't do without -- if even that
    // fails (e.g. the GPU fell off the bus), keep the previous values, the same
    // "leave old values in place" behavior a malformed nvidia-smi line had.
    nvmlUtilization_t util;
    if (g_nvml.get_utilization(g_nvml.device, &util) != 0) {
        return;
    }
    g_gpu_percent = (int)util.gpu;
    g_gpu_mem_percent = (int)util.memory;

    // Everything else is best-effort per field: a card without a fan (or a
    // driver refusing one query) shouldn't blank the whole module.
    unsigned int value = 0;
    g_gpu_temp_c = g_nvml.get_temperature(g_nvml.device, NVML_TEMPERATURE_GPU, &value) == 0 ? (int)value : 0;
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

    g_gpu_available = true;
}

void sysmon_client_poll(void) {
    poll_cpu();
    poll_mem();
    poll_gpu();
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

bool sysmon_gpu_available(void) { return g_gpu_available; }
int sysmon_gpu_percent(void) { return g_gpu_percent; }
int sysmon_gpu_mem_percent(void) { return g_gpu_mem_percent; }
int sysmon_gpu_temp_c(void) { return g_gpu_temp_c; }
int sysmon_gpu_mem_used_mb(void) { return g_gpu_mem_used_mb; }
int sysmon_gpu_mem_total_mb(void) { return g_gpu_mem_total_mb; }
int sysmon_gpu_fan_percent(void) { return g_gpu_fan_percent; }
double sysmon_gpu_power_w(void) { return g_gpu_power_w; }
const char *sysmon_gpu_pstate(void) { return g_gpu_pstate; }

int sysmon_mem_percent(void) { return g_mem_percent; }
double sysmon_mem_used_gb(void) { return g_mem_used_gb; }
double sysmon_mem_total_gb(void) { return g_mem_total_gb; }
double sysmon_mem_free_gb(void) { return g_mem_free_gb; }
double sysmon_swap_used_gb(void) { return g_swap_used_gb; }
double sysmon_swap_total_gb(void) { return g_swap_total_gb; }
int sysmon_swap_percent(void) { return g_swap_percent; }
