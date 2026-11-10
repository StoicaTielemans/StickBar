#include "network/network_client.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util/file.h"
#include "util/history.h"

#define MAX_DNS_SERVERS 8 // /etc/resolv.conf can list more, but glibc's own
                           // resolver only honors the first 3 anyway -- 8 is
                           // generous headroom over that, matching this
                           // project's usual bounded-array convention

static char g_interface_name[64] = "";
static bool g_connected = false;
static bool g_is_wifi = false;
static char g_ip_address[64] = "";
static char g_gateway[64] = "";
static char g_mac_address[32] = "";
static char g_mtu[32] = "";
static char g_link_speed[32] = "";
static char g_duplex[32] = "";

static double g_rx_rate = 0.0, g_tx_rate = 0.0;
static double g_rx_total = 0.0, g_tx_total = 0.0;
static double g_prev_rx = -1.0, g_prev_tx = -1.0;
static double g_prev_sample_time = -1.0;

// Rate history for the dropdown's graph: a ring buffer of the last
// NETWORK_HISTORY_LEN rx/tx rates, one per poll (so 10 minutes at the 5s
// poll). Recorded always, not only while the dropdown is open, so the graph
// is already full the moment it opens -- it costs one store per poll on a
// timer that runs anyway.
static History2 g_hist; // a = rx, b = tx

static char g_dns_servers[MAX_DNS_SERVERS][64];
static int g_dns_count = 0;

static double read_stat_bytes(const char *iface, const char *stat_name) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/%s", iface, stat_name);
    char buf[64];
    if (!file_read_trimmed(path, buf, sizeof(buf)) || buf[0] == '\0') {
        return -1.0;
    }
    return atof(buf);
}

// /proc/net/route's Destination/Gateway columns are 8-hex-digit big fields
// storing an IPv4 address as 4 raw bytes in the kernel's own wire order --
// confirmed against this machine's own real routing table (gateway hex
// "0100A8C0" reverses to 192.168.0.1, matching `ip route show default`
// exactly) rather than assumed from spec text alone. Same reversed-byte-pair
// algorithm as the reference NetworkMonitor.qml's own hexToIp(), just in C.
static void hex_to_ip(const char *hex, char *out, size_t out_size) {
    out[0] = '\0';
    if (!hex || strlen(hex) != 8) {
        return;
    }
    unsigned int b[4];
    for (int i = 0; i < 4; i++) {
        char byte_str[3] = { hex[i * 2], hex[i * 2 + 1], '\0' };
        b[i] = (unsigned int)strtoul(byte_str, NULL, 16);
    }
    snprintf(out, out_size, "%u.%u.%u.%u", b[3], b[2], b[1], b[0]);
}

// Finds the interface whose /proc/net/route entry has an all-zero
// Destination (i.e. the default route), same rule the reference's own
// findDefaultRoute() uses. Returns false if there's no default route at all
// (offline, or a route table with nothing but link-local entries).
static bool find_default_route(char *iface_out, size_t iface_size, char *gateway_hex_out, size_t gw_size) {
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) {
        return false;
    }
    char line[512];
    fgets(line, sizeof(line), f); // header row
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        char iface[64] = "", dest[32] = "", gw[32] = "";
        if (sscanf(line, "%63s %31s %31s", iface, dest, gw) >= 3 && strcmp(dest, "00000000") == 0) {
            snprintf(iface_out, iface_size, "%s", iface);
            snprintf(gateway_hex_out, gw_size, "%s", gw);
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

static bool check_is_wifi(const char *iface) {
    FILE *f = fopen("/proc/net/wireless", "r");
    if (!f) {
        return false;
    }
    char prefix[80];
    snprintf(prefix, sizeof(prefix), "%s:", iface);
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (strncmp(p, prefix, strlen(prefix)) == 0) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

static void refresh_dns_servers(void) {
    g_dns_count = 0;
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (!f) {
        return;
    }
    char line[256];
    while (g_dns_count < MAX_DNS_SERVERS && fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (strncmp(p, "nameserver", 10) != 0) {
            continue;
        }
        p += 10;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        char server[64];
        if (sscanf(p, "%63s", server) == 1) {
            snprintf(g_dns_servers[g_dns_count], sizeof(g_dns_servers[0]), "%s", server);
            g_dns_count++;
        }
    }
    fclose(f);
}

// getifaddrs() (POSIX libc, no subprocess) rather than shelling out to
// `ip -4 addr show dev <iface>` the way NetworkMonitor.qml does -- a
// deliberate improvement, not just a stylistic preference: shelling out
// would mean interpolating a kernel-reported interface name into a command
// string passed through popen()'s /bin/sh, which is both an unnecessary
// process-spawn cost the reference itself only avoids re-running on an
// actual interface change (see its own `ifaceChanged` gate) and a marginally
// riskier pattern than a plain libc call needs to be at all here. Called on
// every poll rather than gated on an interface change like the reference's
// own subprocess -- getifaddrs() is cheap enough (a single syscall
// enumerating already-kernel-resident interface data) that the
// only-on-change optimization isn't needed to justify calling it.
static void refresh_ip_address(const char *iface) {
    g_ip_address[0] = '\0';
    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) != 0) {
        return;
    }
    for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (!ifa->ifa_name || strcmp(ifa->ifa_name, iface) != 0) {
            continue;
        }
        struct sockaddr_in *sa = (struct sockaddr_in *)(void *)ifa->ifa_addr;
        inet_ntop(AF_INET, &sa->sin_addr, g_ip_address, sizeof(g_ip_address));
        break;
    }
    freeifaddrs(ifaddr);
}

void network_client_poll(void) {
    char iface[64] = "";
    char gw_hex[32] = "";
    bool has_route = find_default_route(iface, sizeof(iface), gw_hex, sizeof(gw_hex));

    bool iface_changed = strcmp(iface, g_interface_name) != 0;
    if (iface_changed) {
        snprintf(g_interface_name, sizeof(g_interface_name), "%s", iface);
        g_prev_rx = -1.0;
        g_prev_tx = -1.0;
        // Same reason as the rate reset: samples from the old interface
        // don't belong on the new one's graph.
        history2_reset(&g_hist);
    }

    if (!has_route) {
        g_connected = false;
        g_is_wifi = false;
        g_gateway[0] = '\0';
        g_ip_address[0] = '\0';
        g_rx_rate = 0.0;
        g_tx_rate = 0.0;
        history2_reset(&g_hist);
        return;
    }

    hex_to_ip(gw_hex, g_gateway, sizeof(g_gateway));

    char path[256];
    char buf[128];

    snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", iface);
    file_read_trimmed(path, buf, sizeof(buf));
    g_connected = strcmp(buf, "up") == 0;

    g_is_wifi = check_is_wifi(iface);

    snprintf(path, sizeof(path), "/sys/class/net/%s/address", iface);
    file_read_trimmed(path, g_mac_address, sizeof(g_mac_address));

    snprintf(path, sizeof(path), "/sys/class/net/%s/mtu", iface);
    file_read_trimmed(path, g_mtu, sizeof(g_mtu));

    // "speed" doesn't exist (or reads back <= 0) for most Wi-Fi interfaces --
    // matches the reference's own `speed > 0 ? String(speed) : ""` check.
    snprintf(path, sizeof(path), "/sys/class/net/%s/speed", iface);
    if (file_read_trimmed(path, buf, sizeof(buf)) && atoi(buf) > 0) {
        // Explicit ".*s" precision (not a plain "%s") so gcc's
        // -Wformat-truncation can statically verify this can't overflow
        // g_link_speed, even though `buf` (128 bytes) is wider -- the speed
        // value itself is always a short integer string in practice.
        snprintf(g_link_speed, sizeof(g_link_speed), "%.*s", (int)sizeof(g_link_speed) - 1, buf);
    } else {
        g_link_speed[0] = '\0';
    }

    snprintf(path, sizeof(path), "/sys/class/net/%s/duplex", iface);
    file_read_trimmed(path, g_duplex, sizeof(g_duplex));

    refresh_dns_servers();
    refresh_ip_address(iface);

    double rx = read_stat_bytes(iface, "rx_bytes");
    double tx = read_stat_bytes(iface, "tx_bytes");
    double now = monotonic_seconds();

    if (g_prev_rx >= 0.0 && g_prev_sample_time > 0.0 && rx >= 0.0 && tx >= 0.0) {
        double elapsed = now - g_prev_sample_time;
        if (elapsed > 0.0) {
            double rx_delta = rx - g_prev_rx;
            double tx_delta = tx - g_prev_tx;
            g_rx_rate = rx_delta > 0.0 ? rx_delta / elapsed : 0.0;
            g_tx_rate = tx_delta > 0.0 ? tx_delta / elapsed : 0.0;
        }
    }

    if (rx >= 0.0) {
        g_rx_total = rx;
        g_prev_rx = rx;
    }
    if (tx >= 0.0) {
        g_tx_total = tx;
        g_prev_tx = tx;
    }
    g_prev_sample_time = now;
    history2_push(&g_hist, g_rx_rate, g_tx_rate);
}

bool network_connected(void) { return g_connected; }
bool network_is_wifi(void) { return g_is_wifi; }
const char *network_interface_name(void) { return g_interface_name; }
const char *network_ip_address(void) { return g_ip_address; }
const char *network_gateway(void) { return g_gateway; }
const char *network_mac_address(void) { return g_mac_address; }
const char *network_mtu(void) { return g_mtu; }
const char *network_link_speed(void) { return g_link_speed; }
const char *network_duplex(void) { return g_duplex; }
double network_rx_rate_bytes_per_sec(void) { return g_rx_rate; }
double network_tx_rate_bytes_per_sec(void) { return g_tx_rate; }
double network_rx_total_bytes(void) { return g_rx_total; }
double network_tx_total_bytes(void) { return g_tx_total; }
int network_history_len(void) { return g_hist.len; }
double network_rx_history(int index) { return history2_a(&g_hist, index); }
double network_tx_history(int index) { return history2_b(&g_hist, index); }

int network_dns_count(void) { return g_dns_count; }
const char *network_dns_get(int index) {
    return (index >= 0 && index < g_dns_count) ? g_dns_servers[index] : "";
}
