"""Shared helpers for the live tests in tests/.

Every test here runs a *second* stickBar instance next to whatever bar is already
running (safe -- see CLAUDE.md's "Running alongside Quickshell is safe"), observes
it from the outside (/proc, WAYLAND_DEBUG output, exit status), then stops it.
Nothing here patches or rebuilds the source, so the tests exercise exactly the
binary that ships.

Standard library only, so the tests run with a plain `python3` and no venv.
"""

import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_BIN = REPO / "build" / "stickBar"

# One line per Wayland request/event under WAYLAND_DEBUG=1, prefixed with a
# wall-clock timestamp, e.g.:
#   [14:28:18.001070] {Default Queue}  -> wl_surface#13.commit()
# libwayland 1.2x stamps these from CLOCK_REALTIME, which is what lets
# test_clock_alignment.py check sub-second timing without instrumenting the bar.
_WL_LINE = re.compile(r"^\[(\d+):(\d+):(\d+)\.(\d+)\].*?wl_surface#(\d+)\.(\w+)\(([^)]*)\)")


def bar_binary(argv=None):
    """Binary under test: `--bin PATH` on the command line, else build/stickBar."""
    argv = sys.argv if argv is None else argv
    if "--bin" in argv:
        return Path(argv[argv.index("--bin") + 1]).resolve()
    return DEFAULT_BIN


def require_binary(path):
    if not path.is_file():
        fail(f"{path} not found -- run `meson compile -C build` first")


def require_session():
    if not os.environ.get("WAYLAND_DISPLAY") or not os.environ.get("XDG_RUNTIME_DIR"):
        skip("needs a running Wayland session (WAYLAND_DISPLAY / XDG_RUNTIME_DIR unset)")


def start_bar(binary, stderr_path=None, env_extra=None, wayland_debug=False):
    """Starts a bar instance directly (not via `timeout`/a shell, so the Popen's
    pid really is the bar's own pid -- watching a wrapper's pid instead was a
    real mistake made while writing these)."""
    env = dict(os.environ)
    if wayland_debug:
        env["WAYLAND_DEBUG"] = "1"
    if env_extra:
        env.update(env_extra)
    err = open(stderr_path, "w") if stderr_path else subprocess.DEVNULL
    return subprocess.Popen([str(binary)], stderr=err, stdout=subprocess.DEVNULL, env=env)


def stop_bar(proc, timeout=5):
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def parse_wayland_log(path):
    """Yields (seconds_since_midnight_float, surface_id, request, args) for every
    wl_surface request line in a WAYLAND_DEBUG log."""
    with open(path, errors="replace") as f:
        for line in f:
            m = _WL_LINE.match(line)
            if not m:
                continue
            h, mi, s, frac, sid, req, args = m.groups()
            t = int(h) * 3600 + int(mi) * 60 + int(s) + int(frac) / 10 ** len(frac)
            yield t, int(sid), req, args


def proc_cpu_ticks(pid):
    """(own utime+stime, waited-children cutime+cstime), in clock ticks.

    The children part matters: CPU spent in spawned subprocesses (e.g. the old
    build's nvidia-smi) is only ever charged there, never to the parent's own
    utime/stime -- leaving it out once made the old build look ~3x cheaper
    than it really was."""
    fields = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
    return int(fields[11]) + int(fields[12]), int(fields[13]) + int(fields[14])


def proc_memory_mb(pid):
    """RSS / PSS / Private_Dirty in MB, from /proc/<pid>/smaps_rollup."""
    out = {}
    for line in open(f"/proc/{pid}/smaps_rollup"):
        parts = line.split()
        if parts[0] in ("Rss:", "Pss:", "Private_Dirty:"):
            out[parts[0][:-1]] = int(parts[1]) / 1024
    return out


def wait(seconds):
    time.sleep(seconds)


def ok(msg):
    print(f"PASS  {msg}")
    sys.exit(0)


def fail(msg):
    print(f"FAIL  {msg}")
    sys.exit(1)


def skip(msg):
    # Exit code 77 is the conventional "skipped" status (meson/automake use it).
    print(f"SKIP  {msg}")
    sys.exit(77)
