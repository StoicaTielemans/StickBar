#!/usr/bin/env python3
"""Measures the bar's CPU, memory and compositor load -- optionally side by side
with another build -- and prints a table. Not pass/fail: a measuring tool.

    python3 tests/perf_compare.py                     # build/stickBar alone, 60s
    python3 tests/perf_compare.py --against HEAD~1    # vs. a git ref, built in a temp worktree
    python3 tests/perf_compare.py --against /path/to/other/stickBar --seconds 90

Both instances start fresh at the same time (RSS of a long-running bar drifts
down over hours as pages get reclaimed, so only fresh-vs-fresh is fair).

- CPU: /proc/<pid>/stat utime+stime *plus* cutime+cstime -- spawned children
  (the old build's nvidia-smi) only show up in the latter.
- Memory: /proc/<pid>/smaps_rollup. Private_Dirty is what the bar alone costs;
  RSS includes shared libraries; PSS splits those across their users.
- Compositor load: commits and damaged pixels, counted from WAYLAND_DEBUG
  output. Debug logging itself costs CPU, so it runs as a *separate* pass after
  the CPU/memory pass, never during it.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402

WARMUP_S = 6
TICKS_PER_S = os.sysconf("SC_CLK_TCK")


def build_ref(ref, workdir):
    """Builds git ref `ref` in a temporary worktree; returns its binary path."""
    tree = Path(workdir) / "tree"
    subprocess.run(["git", "-C", str(common.REPO), "worktree", "add", "--detach", str(tree), ref],
                   check=True, capture_output=True)
    subprocess.run(["meson", "setup", "build"], cwd=tree, check=True, capture_output=True)
    subprocess.run(["meson", "compile", "-C", "build"], cwd=tree, check=True, capture_output=True)
    return tree, tree / "build" / "stickBar"


def measure_cpu_mem(binaries, seconds):
    procs = [common.start_bar(b) for b in binaries]
    try:
        time.sleep(WARMUP_S)
        before = [common.proc_cpu_ticks(p.pid) for p in procs]
        time.sleep(seconds)
        after = [common.proc_cpu_ticks(p.pid) for p in procs]
        mem = [common.proc_memory_mb(p.pid) for p in procs]
        threads = [len(os.listdir(f"/proc/{p.pid}/task")) for p in procs]
    finally:
        for p in procs:
            common.stop_bar(p)
    rows = []
    for (s0, c0), (s1, c1), m, th in zip(before, after, mem, threads):
        own = (s1 - s0) / TICKS_PER_S / seconds * 100
        kids = (c1 - c0) / TICKS_PER_S / seconds * 100
        rows.append({"cpu_own": own, "cpu_children": kids, "cpu_total": own + kids,
                     "rss": m["Rss"], "pss": m["Pss"], "private": m["Private_Dirty"], "threads": th})
    return rows


def measure_compositor(binaries, seconds):
    with tempfile.TemporaryDirectory() as tmp:
        logs = [Path(tmp) / f"wl{i}.log" for i in range(len(binaries))]
        procs = [common.start_bar(b, stderr_path=log, wayland_debug=True) for b, log in zip(binaries, logs)]
        time.sleep(WARMUP_S + seconds)
        for p in procs:
            common.stop_bar(p)
        rows = []
        for log in logs:
            entries = list(common.parse_wayland_log(log))
            t0 = entries[0][0] if entries else 0
            commits, px = 0, 0
            for t, _sid, req, args in entries:
                if t - t0 < WARMUP_S:
                    continue
                if req == "commit":
                    commits += 1
                elif req == "damage_buffer":
                    _x, _y, w, h = (int(v) for v in args.split(","))
                    px += w * h
            per_min = 60.0 / seconds
            rows.append({"commits_min": commits * per_min, "px_min": px * per_min,
                         "px_per_commit": px / commits if commits else 0})
        return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", default=str(common.DEFAULT_BIN), help="binary under test (default build/stickBar)")
    ap.add_argument("--against", help="git ref (built in a temp worktree) or path to another stickBar binary")
    ap.add_argument("--seconds", type=int, default=60, help="measurement window per pass (default 60)")
    ap.add_argument("--no-compositor", action="store_true", help="skip the WAYLAND_DEBUG pass")
    args = ap.parse_args()

    common.require_session()
    binaries, labels = [Path(args.bin)], ["this"]
    tmp = tempfile.mkdtemp(prefix="stickbar-perf-")
    worktree = None
    try:
        if args.against:
            other = Path(args.against)
            if not other.is_file():
                print(f"building {args.against} in a temporary worktree...", file=sys.stderr)
                worktree, other = build_ref(args.against, tmp)
            binaries.insert(0, other)
            labels.insert(0, args.against)
        for b in binaries:
            common.require_binary(b)

        print(f"CPU/memory pass ({args.seconds}s)...", file=sys.stderr)
        cpu = measure_cpu_mem(binaries, args.seconds)
        comp = None
        if not args.no_compositor:
            print(f"compositor pass ({args.seconds}s, WAYLAND_DEBUG)...", file=sys.stderr)
            comp = measure_compositor(binaries, args.seconds)

        width = max(14, *(len(label) for label in labels))
        print(f"\n{'':34s}" + "".join(f"{label:>{width}s}" for label in labels))
        lines = [
            ("CPU, own (%)", "cpu_own", "{:.2f}", cpu),
            ("CPU, spawned children (%)", "cpu_children", "{:.2f}", cpu),
            ("CPU, total (%)", "cpu_total", "{:.2f}", cpu),
            ("Memory, private (MB)", "private", "{:.1f}", cpu),
            ("Memory, PSS (MB)", "pss", "{:.1f}", cpu),
            ("Memory, RSS (MB)", "rss", "{:.1f}", cpu),
            ("Threads", "threads", "{:d}", cpu),
        ]
        if comp:
            lines += [
                ("Commits / min", "commits_min", "{:.0f}", comp),
                ("Damaged pixels / min", "px_min", "{:,.0f}", comp),
                ("Avg damaged pixels / commit", "px_per_commit", "{:,.0f}", comp),
            ]
        for title, key, fmt, rows in lines:
            print(f"{title:34s}" + "".join(f"{fmt.format(r[key]):>{width}s}" for r in rows))
    finally:
        if worktree:
            subprocess.run(["git", "-C", str(common.REPO), "worktree", "remove", "--force", str(worktree)],
                           capture_output=True)
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
