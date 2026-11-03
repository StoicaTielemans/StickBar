#!/usr/bin/env python3
"""NVML values must match nvidia-smi's, using the same calls the bar makes.

Mirrors sysmon/sysmon_client.c's NVML usage (same symbols, same struct
layouts, same unit conversions) via ctypes and compares against nvidia-smi.
Mainly guards the memory figure: NVML's v1 memory call counts driver-reserved
memory (~300 MiB more here) and disagrees with nvidia-smi; v2 matches.

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
    calls = {
        "temperature": nvml.nvmlDeviceGetTemperature(dev, 0, C.byref(temp)),
        "memory_v2": nvml.nvmlDeviceGetMemoryInfo_v2(dev, C.byref(mem)),
        "fan": nvml.nvmlDeviceGetFanSpeed(dev, C.byref(fan)),
        "power": nvml.nvmlDeviceGetPowerUsage(dev, C.byref(power)),
        "pstate": nvml.nvmlDeviceGetPerformanceState(dev, C.byref(pstate)),
    }
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
    }


def smi_values():
    if not shutil.which("nvidia-smi"):
        common.skip("nvidia-smi not installed")
    out = subprocess.run(
        ["nvidia-smi", "--query-gpu=temperature.gpu,memory.used,memory.total,fan.speed,power.draw,pstate",
         "--format=csv,noheader,nounits", "-i", "0"],
        capture_output=True, text=True, check=True).stdout.strip()
    temp, used, total, fan, power, pstate = [x.strip() for x in out.split(",")]
    return {"temp": int(temp), "mem_used": int(used), "mem_total": int(total),
            "fan": int(fan) if fan.isdigit() else 0, "power": float(power), "pstate": pstate}


def main():
    ours = nvml_values()
    smi = smi_values()
    bad = []
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
    common.ok(f"NVML matches nvidia-smi ({summary})")


if __name__ == "__main__":
    main()
