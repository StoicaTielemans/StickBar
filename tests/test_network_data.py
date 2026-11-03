#!/usr/bin/env python3
"""The network dropdown's details match `ip` and /sys.

Reads the bar's network client (test-control `dump net`, after a `refresh
poll`) and compares: default-route interface and gateway (`ip route show
default`), IPv4 address (`ip -4 -o addr show`), MAC and MTU
(/sys/class/net/<if>/), DNS servers (/etc/resolv.conf), and the rx/tx
totals, which must fall between the kernel counters read just before and
just after the bar's own read. Also checks the rate history grows by one
sample per poll.
"""

import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402


def sh(*args):
    return subprocess.run(args, capture_output=True, text=True).stdout


def counters(iface):
    base = Path(f"/sys/class/net/{iface}/statistics")
    return int((base / "rx_bytes").read_text()), int((base / "tx_bytes").read_text())


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)
    routes = json.loads(sh("ip", "-j", "route", "show", "default") or "[]")
    if not routes:
        common.skip("no default route")
    iface, gateway = routes[0]["dev"], routes[0].get("gateway", "")
    failures = []

    with common.ControlledBar(binary) as bar:
        bar.wait_ready(("network",))
        before = counters(iface)
        h0 = bar.must("dump net")["history_len"]
        bar.must("refresh poll")
        n = bar.must("dump net")
        after = counters(iface)

    if n["iface"] != iface:
        failures.append(f"iface {n['iface']!r} vs {iface!r}")
    if n["gateway"] != gateway:
        failures.append(f"gateway {n['gateway']!r} vs {gateway!r}")
    addrs = json.loads(sh("ip", "-j", "-4", "addr", "show", "dev", iface) or "[]")
    ips = [a["local"] for entry in addrs for a in entry.get("addr_info", [])]
    if n["ip"] not in ips:
        failures.append(f"ip {n['ip']!r} not among {ips}")
    mac = Path(f"/sys/class/net/{iface}/address").read_text().strip()
    if n["mac"].lower() != mac.lower():
        failures.append(f"mac {n['mac']!r} vs {mac!r}")
    mtu = Path(f"/sys/class/net/{iface}/mtu").read_text().strip()
    if n["mtu"] != mtu:
        failures.append(f"mtu {n['mtu']!r} vs {mtu!r}")
    dns = [line.split()[1] for line in open("/etc/resolv.conf")
           if line.startswith("nameserver") and len(line.split()) > 1][:8]
    if n["dns"] != dns:
        failures.append(f"dns {n['dns']} vs {dns}")
    for i, key in enumerate(("rx_total", "tx_total")):
        if not before[i] <= n[key] <= after[i]:
            failures.append(f"{key} {n[key]:.0f} outside kernel counters {before[i]}..{after[i]}")
    if n["history_len"] != min(h0 + 1, 120):
        failures.append(f"history {h0} -> {n['history_len']} after one poll")
    if n["rx_rate"] < 0 or n["tx_rate"] < 0:
        failures.append("negative rate")

    if failures:
        common.fail("; ".join(failures))
    common.ok(f"{iface} via {gateway}, {n['ip']}, mac/mtu/dns match; totals within kernel counters; "
              f"history {h0} -> {n['history_len']}")


if __name__ == "__main__":
    main()
