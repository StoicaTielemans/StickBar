#!/usr/bin/env python3
"""The CPU and memory dropdowns' numbers match the kernel's.

Reads what the bar's sysmon client holds (through the test-control channel's
`dump`, right after a `refresh` -- what an open dropdown does) and compares
it with /proc and /sys read at the same moment:

Memory: apps (total - available), MemFree, MemAvailable, Shmem, swap; PSI
present iff /proc/pressure/memory exists; zram present iff /proc/swaps lists
a /dev/zramN, with its algorithm and mm_stat sizes. Top apps: a child process
holding ~400 MB of touched memory must show up under its own name with at
least that much *private* memory.

CPU: model, threads, physical cores, max clock, the hybrid P/E split (sysfs
cpu_core/cpu_atom), uptime. Top processes: two busy loops must show up as
`sh` at about 2/threads of the whole CPU, measured over a fresh two-scan
window.
"""

import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402

ALLOC_MB = 400
BUSY_LOOPS = 2
SAMPLE_S = 2.0


def meminfo():
    out = {}
    for line in open("/proc/meminfo"):
        key, value = line.split(":", 1)
        out[key] = int(value.split()[0]) / 1024 / 1024  # GiB, like the bar
    return out


def cpulist(path):
    try:
        text = open(path).read().strip()
    except OSError:
        return None
    cpus = set()
    for part in text.split(","):
        lo, _, hi = part.partition("-")
        cpus.update(range(int(lo), int(hi or lo) + 1))
    return cpus


def near(a, b, tol):
    return abs(a - b) <= tol


def check_memory(bar, failures):
    bar.must("refresh mem")
    m = bar.must("dump mem")
    k = meminfo()
    checks = [
        ("apps_gb", k["MemTotal"] - k["MemAvailable"], 0.25),
        ("free_gb", k["MemFree"], 0.25),
        ("avail_gb", k["MemAvailable"], 0.25),
        ("total_gb", k["MemTotal"], 0.001),
        ("shared_gb", k["Shmem"], 0.1),
        ("swap_total_gb", k["SwapTotal"], 0.001),
        ("swap_used_gb", k["SwapTotal"] - k["SwapFree"], 0.1),
    ]
    for key, expected, tol in checks:
        if not near(m[key], expected, tol):
            failures.append(f"mem {key} {m[key]:.3f} vs kernel {expected:.3f}")
    if abs(m["apps_gb"] + m["cache_gb"] + m["free_gb"] - m["total_gb"]) > 0.01:
        failures.append("apps + cache + free doesn't add up to the total")

    if m["psi"] != os.path.exists("/proc/pressure/memory"):
        failures.append(f"psi {m['psi']} but /proc/pressure/memory exists={not m['psi']}")
    zram_dev = next((line.split()[0][5:] for line in open("/proc/swaps").readlines()[1:]
                     if line.startswith("/dev/zram")), None)
    if m["zram"] != (zram_dev is not None):
        failures.append(f"zram {m['zram']} but /proc/swaps zram device: {zram_dev}")
    if zram_dev:
        algo = re.search(r"\[(\w+)\]", open(f"/sys/block/{zram_dev}/comp_algorithm").read())
        if algo and m["zram_algorithm"] != algo.group(1):
            failures.append(f"zram algorithm {m['zram_algorithm']!r} vs {algo.group(1)!r}")
        orig, compr = (int(v) / 1048576 for v in open(f"/sys/block/{zram_dev}/mm_stat").read().split()[:2])
        if not near(m["zram_orig_mb"], orig, max(8, orig * 0.05)):
            failures.append(f"zram stored {m['zram_orig_mb']:.0f} MB vs mm_stat {orig:.0f} MB")
        if not near(m["zram_compr_mb"], compr, max(4, compr * 0.05)):
            failures.append(f"zram compressed {m['zram_compr_mb']:.0f} MB vs mm_stat {compr:.0f} MB")
    return m


def check_memory_apps(bar, failures):
    # A child that touches every page of ALLOC_MB, then idles. Named by a
    # distinctive comm (the kernel truncates to 15 chars) so it can't merge
    # with the user's own processes.
    child = subprocess.Popen(
        [sys.executable, "-c",
         "import ctypes,time; ctypes.CDLL(None).prctl(15, b'stickbar_memtst', 0, 0, 0); "
         f"b = bytearray({ALLOC_MB} * 1024 * 1024); b[::4096] = b'x' * len(b[::4096]); "
         "print('ready', flush=True); time.sleep(30)"],
        stdout=subprocess.PIPE, text=True)
    try:
        child.stdout.readline()
        bar.must("refresh mem")
        procs = bar.must("dump mem")["procs"]
    finally:
        child.kill()
        child.wait()
    ours = next((p for p in procs if p["name"] == "stickbar_memtst"), None)
    if ours is None:
        failures.append(f"a {ALLOC_MB} MB process isn't in the top apps: {procs}")
        return "top apps: missing"
    if ours["gb"] * 1024 < ALLOC_MB * 0.95:
        failures.append(f"{ALLOC_MB} MB process shows {ours['gb'] * 1024:.0f} MB")
    return f"{ALLOC_MB} MB child -> {ours['gb'] * 1024:.0f} MB"


def check_cpu(bar, failures):
    c = bar.must("dump cpu")
    info = open("/proc/cpuinfo").read()
    model = re.search(r"^model name\s*:\s*(.+)$", info, re.M).group(1).strip()
    if c["model"] != model:
        failures.append(f"cpu model {c['model']!r} vs {model!r}")
    if c["threads"] != os.cpu_count():
        failures.append(f"threads {c['threads']} vs {os.cpu_count()}")
    cores = re.search(r"^cpu cores\s*:\s*(\d+)", info, re.M)
    if cores and c["physical_cores"] != int(cores.group(1)):
        failures.append(f"physical cores {c['physical_cores']} vs {cores.group(1)}")
    try:
        max_khz = int(open("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq").read())
        if not near(c["max_ghz"], max_khz / 1e6, 0.01):
            failures.append(f"max {c['max_ghz']} GHz vs {max_khz / 1e6}")
    except OSError:
        pass
    p_cores = cpulist("/sys/devices/cpu_core/cpus")
    e_cores = cpulist("/sys/devices/cpu_atom/cpus")
    hybrid = p_cores is not None and e_cores is not None
    if c["hybrid"] != hybrid:
        failures.append(f"hybrid {c['hybrid']} vs sysfs {hybrid}")
    if hybrid:
        got = {i for i, eff in enumerate(c["efficient"]) if eff}
        if got != e_cores:
            failures.append(f"E-cores {sorted(got)} vs sysfs {sorted(e_cores)}")
    bar.must("refresh cpu")
    up = float(open("/proc/uptime").read().split()[0])
    c = bar.must("dump cpu")
    if not near(c["uptime_s"], up, 2):
        failures.append(f"uptime {c['uptime_s']:.0f}s vs {up:.0f}s")
    if c["tasks_total"] <= 0:
        failures.append("no task count")
    return f"{c['model']}, {c['threads']} threads, hybrid={c['hybrid']}"


def check_cpu_procs(bar, failures):
    loops = [subprocess.Popen(["sh", "-c", "while :; do :; done"]) for _ in range(BUSY_LOOPS)]
    try:
        time.sleep(0.5)
        bar.must("refresh cpu-reset")  # primes a fresh window
        time.sleep(SAMPLE_S)
        bar.must("refresh cpu")
        c = bar.must("dump cpu")
    finally:
        for p in loops:
            p.kill()
            p.wait()
    expected = 100.0 * BUSY_LOOPS / os.cpu_count()
    sh = next((p for p in c["procs"] if p["name"] == "sh"), None)
    if not c["procs_ready"] or sh is None:
        failures.append(f"{BUSY_LOOPS} busy loops not in the top processes: {c['procs']}")
        return "top processes: missing"
    # Loose: a loaded desktop (or a game) can take some of their time.
    if not (expected * 0.6 <= sh["percent"] <= expected * 1.25):
        failures.append(f"{BUSY_LOOPS} busy loops = {sh['percent']:.1f}%, expected ~{expected:.1f}%")
    return f"{BUSY_LOOPS} busy loops -> sh {sh['percent']:.1f}% (expected ~{expected:.1f}%)"


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)
    failures = []
    with common.ControlledBar(binary) as bar:
        bar.wait_ready(("cpu", "mem"))
        m = check_memory(bar, failures)
        apps = check_memory_apps(bar, failures)
        cpu = check_cpu(bar, failures)
        procs = check_cpu_procs(bar, failures)
    if failures:
        common.fail("; ".join(failures))
    common.ok(f"memory matches /proc (zram={m['zram']}, psi={m['psi']}); {apps}; {cpu}; {procs}")


if __name__ == "__main__":
    main()
