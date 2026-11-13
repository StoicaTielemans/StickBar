#!/usr/bin/env python3
"""NVML values must match nvidia-smi's, using the same calls the bar makes.

Mirrors sysmon/sysmon_client.c's NVML usage (same symbols, same struct
layouts, same unit conversions) via ctypes and compares against nvidia-smi.
Mainly guards the memory figure: NVML's v1 memory call counts driver-reserved
memory (~300 MiB more here) and disagrees with nvidia-smi; v2 matches. Also
covers the GPU dropdown's extras: name, driver, power limit, max clock, and
the per-process VRAM list (the _v3 process queries and their struct layout).

If you change which NVML calls/structs sysmon_client.c uses, change them here
too. Skips on machines without an NVIDIA driver.
"""

import ctypes as C
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402

# Values that move from moment to moment get a tolerance; static ones must match.
TOLERANCE = {"temp": 3, "mem_used": 64, "power": 15.0}


class ProcessInfo(C.Structure):
    # nvmlProcessInfo_v2_t -- what the _v3 running-process queries fill in.
    _fields_ = [("pid", C.c_uint), ("usedGpuMemory", C.c_ulonglong),
                ("gpuInstanceId", C.c_uint), ("computeInstanceId", C.c_uint)]


def nvml_procs(nvml, dev):
    """PID -> MiB, graphics + compute merged keeping the larger, like sysmon_client.c."""
    merged = {}
    for fn in (nvml.nvmlDeviceGetGraphicsRunningProcesses_v3, nvml.nvmlDeviceGetComputeRunningProcesses_v3):
        buf = (ProcessInfo * 64)()
        count = C.c_uint(64)
        if fn(dev, C.byref(count), buf) != 0:
            common.fail(f"{fn.__name__} failed")
        for p in buf[:count.value]:
            mib = 0 if p.usedGpuMemory == 2**64 - 1 else p.usedGpuMemory // (1024 * 1024)
            merged[p.pid] = max(merged.get(p.pid, 0), mib)
    return merged


class Utilization(C.Structure):
    _fields_ = [("gpu", C.c_uint), ("memory", C.c_uint)]


class MemoryV2(C.Structure):
    _fields_ = [("version", C.c_uint), ("total", C.c_ulonglong), ("reserved", C.c_ulonglong),
                ("free", C.c_ulonglong), ("used", C.c_ulonglong)]


def nvml_values():
    try:
        nvml = C.CDLL("libnvidia-ml.so.1")
    except OSError:
        common.skip("libnvidia-ml.so.1 not found (no NVIDIA driver)")
    if nvml.nvmlInit_v2() != 0:
        common.skip("nvmlInit_v2 failed (NVIDIA driver not loaded)")
    dev = C.c_void_p()
    if nvml.nvmlDeviceGetHandleByIndex_v2(0, C.byref(dev)) != 0:
        common.skip("NVML found no GPU at index 0")

    mem = MemoryV2()
    mem.version = C.sizeof(MemoryV2) | (2 << 24)  # NVML_STRUCT_VERSION(Memory, 2)
    temp, fan, power, pstate = C.c_uint(), C.c_uint(), C.c_uint(), C.c_int()
    name, driver = C.create_string_buffer(96), C.create_string_buffer(80)
    limit, max_clock = C.c_uint(), C.c_uint()
    calls = {
        "temperature": nvml.nvmlDeviceGetTemperature(dev, 0, C.byref(temp)),
        "memory_v2": nvml.nvmlDeviceGetMemoryInfo_v2(dev, C.byref(mem)),
        "fan": nvml.nvmlDeviceGetFanSpeed(dev, C.byref(fan)),
        "power": nvml.nvmlDeviceGetPowerUsage(dev, C.byref(power)),
        "pstate": nvml.nvmlDeviceGetPerformanceState(dev, C.byref(pstate)),
        "name": nvml.nvmlDeviceGetName(dev, name, 96),
        "driver": nvml.nvmlSystemGetDriverVersion(driver, 80),
        "power_limit": nvml.nvmlDeviceGetEnforcedPowerLimit(dev, C.byref(limit)),
        "max_clock": nvml.nvmlDeviceGetMaxClockInfo(dev, 0, C.byref(max_clock)),
    }
    procs = nvml_procs(nvml, dev)
    nvml.nvmlShutdown()
    failed = [k for k, rc in calls.items() if rc != 0 and k != "fan"]  # fanless cards are fine
    if failed:
        common.fail(f"NVML calls failed: {', '.join(failed)}")
    return {
        "temp": temp.value,
        "mem_used": mem.used // (1024 * 1024),
        "mem_total": mem.total // (1024 * 1024),
        "fan": fan.value if calls["fan"] == 0 else 0,
        "power": power.value / 1000.0,
        "pstate": f"P{pstate.value}",
        "name": name.value.decode(),
        "driver": driver.value.decode(),
        "power_limit": round(limit.value / 1000.0, 2),
        "max_clock": max_clock.value,
    }, procs


def smi_values():
    if not shutil.which("nvidia-smi"):
        common.skip("nvidia-smi not installed")
    out = subprocess.run(
        ["nvidia-smi", "--query-gpu=temperature.gpu,memory.used,memory.total,fan.speed,power.draw,pstate,"
                       "name,driver_version,power.limit,clocks.max.graphics",
         "--format=csv,noheader,nounits", "-i", "0"],
        capture_output=True, text=True, check=True).stdout.strip()
    temp, used, total, fan, power, pstate, name, driver, limit, max_clock = [x.strip() for x in out.split(",")]
    # Driver 620's nvidia-smi prints the temperature with decimals ("36.66"),
    # older drivers a whole number; NVML always gives whole degrees.
    return {"temp": round(float(temp)), "mem_used": int(used), "mem_total": int(total),
            "fan": int(fan) if fan.isdigit() else 0, "power": float(power), "pstate": pstate,
            "name": name, "driver": driver, "power_limit": float(limit), "max_clock": int(max_clock)}


def smi_procs():
    """PID -> MiB from nvidia-smi's compute-apps list (graphics-only apps
    aren't in any CSV query, so this is checked as a subset)."""
    out = subprocess.run(["nvidia-smi", "--query-compute-apps=pid,used_memory", "--format=csv,noheader,nounits",
                          "-i", "0"], capture_output=True, text=True, check=True).stdout
    procs = {}
    for line in out.strip().splitlines():
        pid, mib = [x.strip() for x in line.split(",")]
        if mib.isdigit():
            procs[int(pid)] = int(mib)
    return procs


def main():
    ours, our_procs = nvml_values()
    smi = smi_values()
    their_procs = smi_procs()
    bad = []
    # Every compute app nvidia-smi lists must be in NVML's merged list with a
    # similar figure (apps allocate between the two reads; processes that
    # exited in between are skipped).
    for pid, mib in their_procs.items():
        if pid not in our_procs:
            if Path(f"/proc/{pid}").exists():
                bad.append(f"pid {pid} ({mib} MiB) missing from NVML's process list")
        elif abs(our_procs[pid] - mib) > TOLERANCE["mem_used"]:
            bad.append(f"pid {pid}: NVML {our_procs[pid]} MiB vs nvidia-smi {mib} MiB")
    for key, mine in ours.items():
        theirs = smi[key]
        tol = TOLERANCE.get(key)
        if tol is None:
            # P-state can legitimately flip between two adjacent reads.
            if key == "pstate" or key == "fan":
                continue
            if mine != theirs:
                bad.append(f"{key}: NVML {mine} vs nvidia-smi {theirs}")
        elif abs(mine - theirs) > tol:
            bad.append(f"{key}: NVML {mine} vs nvidia-smi {theirs} (tolerance {tol})")
    summary = ", ".join(f"{k}={ours[k]}" for k in ours)
    if bad:
        common.fail("; ".join(bad) + f"  [NVML: {summary}]")
    common.ok(f"NVML matches nvidia-smi ({summary}; {len(their_procs)} compute apps checked)")


if __name__ == "__main__":
    main()
