#ifndef CUSTOMBAR_NETWORK_NETWORK_CLIENT_H
#define CUSTOMBAR_NETWORK_NETWORK_CLIENT_H

#include "util/history.h"

#include <stdbool.h>

// Re-reads /proc/net/route (to find the default-route interface), /proc/net/wireless
// (Wi-Fi vs. Ethernet detection), /sys/class/net/<iface>/{operstate,address,mtu,
// speed,duplex,statistics/{rx,tx}_bytes}, and /etc/resolv.conf, plus a native
// getifaddrs() lookup for the interface's own IPv4 address -- see
// docs/modules/network.md for why that's a getifaddrs() call
// rather than shelling out to `ip addr show` the way the reference
// NetworkMonitor.qml does. No init() call is needed (unlike
// sysmon/sysmon_client.h's sysmon_client_init()) -- the only delta-based
// state here (rx/tx rate) self-primes on its own first poll, matching this
// project's existing convention for a first sample with nothing to diff
// against yet. Call once at startup and then on every tick of the same
// 5-second timerfd main.c already drives sysmon_client_poll() from --
// matches the reference's own `Timer { interval: 5000 }` exactly, and avoids
// adding a second poll source for an identical cadence.
void network_client_poll(void);

// Re-reads the details only the dropdown shows (MAC, MTU, link speed, duplex,
// IP address, DNS servers). The 5s poll does this only when the interface
// changes -- the bar box shows none of it -- so the dropdown calls it when it
// opens and on each refresh while open.
void network_client_refresh_details(void);

bool network_connected(void);
bool network_is_wifi(void);
const char *network_interface_name(void); // "" if no default route at all
const char *network_ip_address(void);     // "" if unknown
const char *network_gateway(void);        // "" if no default route
const char *network_mac_address(void);    // "" if unknown
const char *network_mtu(void);            // "" if unknown
const char *network_link_speed(void);     // Mbps as a string, "" if unknown/not applicable (e.g. most Wi-Fi)
const char *network_duplex(void);         // "" if unknown/not applicable

// Bytes/sec, sampled as a delta between the two most recent polls (0 on the
// very first poll for a given interface -- no prior sample to diff against
// yet, same self-correcting "0 until the next poll" behavior
// sysmon_client's CPU-jiffies delta already has).
double network_rx_rate_bytes_per_sec(void);
double network_tx_rate_bytes_per_sec(void);
double network_rx_total_bytes(void);
double network_tx_total_bytes(void);

// The last NETWORK_HISTORY_LEN rx/tx rates (one per poll, so 10 minutes at
// the 5s poll), for the dropdown's graph. Index 0 is the oldest sample,
// network_history_len() - 1 the newest. Cleared whenever the default-route
// interface changes or goes away, so it can hold fewer than the maximum.
#define NETWORK_HISTORY_LEN HISTORY_LEN
int network_history_len(void);
double network_rx_history(int index);
double network_tx_history(int index);

int network_dns_count(void);
const char *network_dns_get(int index); // 0 <= index < network_dns_count()

#endif
