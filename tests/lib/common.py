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


# ---- Driving a bar through its test-control channel ----------------------------
#
# STICKBAR_TEST_CONTROL=1 turns the bar's stdin into a command channel (see
# src/testing/test_control.c): open dropdowns by injected clicks, click/drag/
# scroll inside them, dump what its data sources hold, render the open
# dropdown to a PNG. Injected input goes through the bar's own pointer
# routing, so no real cursor is moved and nothing on screen is clicked --
# safe to run while the desktop is in use. STICKBAR_TEST_OUTPUT keeps the
# instance (bar *and* popup click-catchers) on one monitor.

import json
import select


def hypr_json(*args):
    out = subprocess.run(["hyprctl", "-j", *args], capture_output=True, text=True).stdout
    try:
        return json.loads(out)
    except ValueError:
        return []


def pick_test_output():
    """A monitor without a fullscreen window on it (so a test popup never
    covers a game or video), preferring the one that isn't focused. Skips the
    test if every monitor has a fullscreen window."""
    busy = {c.get("monitor") for c in hypr_json("clients") if c.get("fullscreen")}
    monitors = [m for m in hypr_json("monitors") if m.get("id") not in busy]
    if not monitors:
        skip("every monitor has a fullscreen window -- not opening test popups over it")
    monitors.sort(key=lambda m: m.get("focused", False))
    return monitors[0]["name"]


class ControlledBar:
    """A second bar instance on one monitor, driven over its stdin channel."""

    def __init__(self, binary, output=None, wayland_debug=False, stderr_path=None):
        self.output = output or pick_test_output()
        env = dict(os.environ, STICKBAR_TEST_CONTROL="1", STICKBAR_TEST_OUTPUT=self.output)
        if wayland_debug:
            env["WAYLAND_DEBUG"] = "1"
        self._err = open(stderr_path, "w") if stderr_path else subprocess.DEVNULL
        self.proc = subprocess.Popen([str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self._err, env=env, text=True, bufsize=1)

    def cmd(self, line, timeout=5.0):
        """Sends one command, returns its JSON reply (a dict)."""
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        ready, _, _ = select.select([self.proc.stdout], [], [], timeout)
        if not ready:
            fail(f"no reply to {line!r} within {timeout}s")
        reply = self.proc.stdout.readline()
        if not reply:
            fail(f"bar exited while handling {line!r} (status {self.proc.poll()})")
        return json.loads(reply)

    def must(self, line, timeout=5.0):
        r = self.cmd(line, timeout)
        if not r.get("ok"):
            fail(f"{line!r} failed: {r.get('error')}")
        return r

    def wait_ready(self, modules=("clock",), timeout=8.0):
        """Waits until the bar has drawn on its output and every module in
        `modules` has a box (some appear only after their data arrives)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            rects = self.cmd(f"rects {self.output}").get("rects", {})
            if all(m in rects for m in modules):
                return rects
            time.sleep(0.2)
        fail(f"bar never drew {modules} on {self.output} (got {list(rects)})")

    def click_module(self, module, button="right"):
        """Clicks the centre of a module's box on the bar."""
        rects = self.wait_ready((module,))
        x, y, w, h = rects[module]
        self.must(f"click {self.output} {x + w / 2:.1f} {y + h / 2:.1f} {button}")

    def wait_menu(self, open_=True, timeout=3.0, **cond):
        """Waits until `menu` reports the popup open (or closed), and every
        key=value in `cond` matches; returns the last reply."""
        deadline = time.time() + timeout
        r = {}
        while time.time() < deadline:
            r = self.must("menu")
            if r["open"] == open_ and all(r.get(k) == v for k, v in cond.items()):
                return r
            time.sleep(0.1)
        fail(f"menu never reached open={open_} {cond}: last {r}")

    def stop(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        stop_bar(self.proc)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()


def png_pixels(path):
    """(width, height, rows) for an 8-bit RGBA/RGB PNG as cairo writes it --
    rows[y][x] = (r, g, b, a). Pure stdlib (zlib), no Pillow needed."""
    import struct
    import zlib
    data = open(path, "rb").read()
    pos, chunks = 8, []
    width = height = color_type = 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, _depth, color_type = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            chunks.append(body)
        pos += 12 + length
    channels = 4 if color_type == 6 else 3
    raw = zlib.decompress(b"".join(chunks))
    stride = width * channels
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(height):
        ftype, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b = prev[x]
            c = prev[x - channels] if x >= channels else 0
            if ftype == 1:
                line[x] = (line[x] + a) & 0xFF
            elif ftype == 2:
                line[x] = (line[x] + b) & 0xFF
            elif ftype == 3:
                line[x] = (line[x] + (a + b) // 2) & 0xFF
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        rows.append([tuple(line[x:x + channels]) + ((255,) if channels == 3 else ())
                     for x in range(0, stride, channels)])
        prev = line
    return width, height, rows


def hex_rgb(value):
    value = value.lstrip("#")
    return tuple(int(value[i:i + 2], 16) for i in (0, 2, 4))


def close_rgb(a, b, tol=12):
    return all(abs(x - y) <= tol for x, y in zip(a[:3], b[:3]))


# Catppuccin Mocha, the subset the tests check pixels against (config.c).
COLORS = {name: hex_rgb(v) for name, v in {
    "mantle": "181825", "base": "1e1e2e", "surface0": "313244", "surface1": "45475a",
    "peach": "fab387", "green": "a6e3a1", "yellow": "f9e2af", "teal": "94e2d5",
    "sky": "89dceb", "blue": "89b4fa", "mauve": "cba6f7",
}.items()}
