#include "sysmon/sysmon_client.h"

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

void sysmon_client_init(void) {
    memset(&g_prev_total, 0, sizeof(g_prev_total));
    memset(g_prev_cores, 0, sizeof(g_prev_cores));
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

// Synchronous (popen()/pclose() block until nvidia-smi exits) -- see
// CLAUDE.md's "System monitor (CPU/GPU/memory)" section for the measured
// latency (~15-45ms on this machine) and why that's an acceptable window for
// a call made once per 5-second poll tick, not per frame or per click.
static void poll_gpu(void) {
    FILE *p = popen("nvidia-smi --query-gpu=utilization.gpu,utilization.memory,temperature.gpu,"
                     "memory.used,memory.total,fan.speed,power.draw,pstate "
                     "--format=csv,noheader,nounits 2>/dev/null",
                     "r");
    if (!p) {
        if (!g_gpu_warned) {
            fprintf(stderr, "gridBar: failed to spawn nvidia-smi -- GPU module will show "
                             "\"unavailable\" for this run\n");
            g_gpu_warned = true;
        }
        return;
    }

    char line[256] = "";
    bool got_line = fgets(line, sizeof(line), p) != NULL;
    int rc = pclose(p);

    if (!got_line || rc != 0) {
        if (!g_gpu_warned) {
            fprintf(stderr, "gridBar: nvidia-smi query failed -- is an NVIDIA GPU/driver "
                             "present? GPU module will show \"unavailable\" for this run\n");
            g_gpu_warned = true;
        }
        return;
    }

    int util = 0, mem_util = 0, temp = 0, mem_used = 0, mem_total = 0, fan = 0;
    double power = 0.0;
    char pstate[16] = "";
    int n = sscanf(line, "%d, %d, %d, %d, %d, %d, %lf, %15s", &util, &mem_util, &temp, &mem_used,
                    &mem_total, &fan, &power, pstate);
    if (n < 8) {
        return; // unexpected output shape -- leave the previous (or default) values in place
    }

    g_gpu_percent = util;
    g_gpu_mem_percent = mem_util;
    g_gpu_temp_c = temp;
    g_gpu_mem_used_mb = mem_used;
    g_gpu_mem_total_mb = mem_total;
    g_gpu_fan_percent = fan;
    g_gpu_power_w = power;
    snprintf(g_gpu_pstate, sizeof(g_gpu_pstate), "%s", pstate);
    g_gpu_available = true;
}

void sysmon_client_poll(void) {
    poll_cpu();
    poll_mem();
    poll_gpu();
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
