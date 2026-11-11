#!/usr/bin/env python3
"""Redraws must be minimal: small damage on clock ticks, nothing for no-ops.

Checks the redraw path in bar/bar_surface.c from the outside, via WAYLAND_DEBUG:

1. Clock-only ticks damage a small rectangle (the changed digits), not the
   whole 1920x20 surface. At least MIN_SMALL commits, and at least a quarter
   of all post-startup commits, must damage under SMALL_DAMAGE_PX. The rest
   are 5s polls and other real changes (a playing Spotify adds its own), so
   this deliberately isn't "most commits": a first version required half and
   failed intermittently while music was playing. The regression it guards
   against is unambiguous anyway -- the old full-redraw build had *zero*
   small commits.
2. Events that change nothing visible produce no commits: fires a burst of
   `pactl` calls setting the default sink's volume/mute to their *current*
   values (each one is a real PulseAudio change event the bar receives) and
   checks the commit count in that window stays at what the clock alone and at
   most one 5s poll would produce. Skipped if pactl isn't available. This does
   touch the real default sink, but only ever sets it to what it already is.

Both checks count on a clock tick every second, so the bar runs with
STICKBAR_TEST_CLOCK_SECONDS (it normally hides seconds and ticks once a minute).
"""

import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402

WARMUP_S = 3
SMALL_DAMAGE_PX = 2000
MIN_SMALL = 3
NOOP_CALLS = 20
NOOP_WINDOW_S = 2.0
CLOCK_SECONDS = {"STICKBAR_TEST_CLOCK_SECONDS": "1"}


def current_sink_state():
    vol = subprocess.run(["pactl", "get-sink-volume", "@DEFAULT_SINK@"], capture_output=True, text=True).stdout
    mute = subprocess.run(["pactl", "get-sink-mute", "@DEFAULT_SINK@"], capture_output=True, text=True).stdout
    m = re.search(r"(\d+)%", vol)
    if not m:
        return None
    return f"{m.group(1)}%", "1" if "yes" in mute else "0"


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)
    have_pactl = shutil.which("pactl") is not None
    state = current_sink_state() if have_pactl else None

    with tempfile.TemporaryDirectory() as tmp:
        log = Path(tmp) / "wl.log"
        bar = common.start_bar(binary, stderr_path=log, wayland_debug=True, env_extra=CLOCK_SECONDS)
        try:
            time.sleep(WARMUP_S + 3)
            burst_start = burst_end = None
            if state:
                burst_start = time.time()
                for _ in range(NOOP_CALLS):
                    subprocess.run(["pactl", "set-sink-volume", "@DEFAULT_SINK@", state[0]])
                    subprocess.run(["pactl", "set-sink-mute", "@DEFAULT_SINK@", state[1]])
                burst_end = burst_start + max(NOOP_WINDOW_S, time.time() - burst_start)
                time.sleep(max(0.0, burst_end - time.time()) + 0.5)
        finally:
            common.stop_bar(bar)
        entries = list(common.parse_wayland_log(log))

    if not entries:
        common.fail("no wl_surface lines in the WAYLAND_DEBUG log")
    t0 = entries[0][0]
    damages = []
    commits = []
    surfaces = set()
    for t, sid, req, args in entries:
        if t - t0 < WARMUP_S:
            continue
        if req == "damage_buffer":
            x, y, w, h = (int(v) for v in args.split(","))
            damages.append(w * h)
            surfaces.add(sid)
        elif req == "commit":
            commits.append(t)

    if not damages:
        common.fail("no damage_buffer requests after warmup")
    small = sum(1 for a in damages if a < SMALL_DAMAGE_PX)
    if small < MIN_SMALL or small * 4 < len(damages):
        common.fail(f"only {small}/{len(damages)} commits had small damage (< {SMALL_DAMAGE_PX}px); "
                    f"largest areas: {sorted(damages)[-5:]}")
    msg = f"{small}/{len(damages)} commits damaged < {SMALL_DAMAGE_PX}px (median {sorted(damages)[len(damages)//2]}px)"

    if state:
        # Map the burst's wall-clock window onto the log's seconds-since-midnight.
        # WAYLAND_DEBUG stamps are UTC (gmtime), not local time -- using
        # localtime here once silently shifted the window by the UTC offset and
        # matched zero commits, making this check vacuous.
        lt = time.gmtime(burst_start)
        offset = lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec + (burst_start % 1)
        span = burst_end - burst_start
        in_window = [t for t in commits if offset <= t <= offset + span]
        n_surf = max(1, len(surfaces))
        allowed = n_surf * (int(span) + 2) + n_surf  # clock ticks + one possible 5s poll
        if not in_window:
            # The clock ticks every second, so an empty window means the window
            # mapping is wrong, not that the bar is quiet -- fail loudly.
            common.fail("no commits at all inside the no-op window (expected clock ticks) -- "
                        "log timestamp mapping is broken")
        if len(in_window) > allowed:
            common.fail(f"{len(in_window)} commits during {NOOP_CALLS * 2} no-op PulseAudio events "
                        f"over {span:.1f}s (allowed {allowed} for clock ticks + one poll)")
        msg += f"; {NOOP_CALLS * 2} no-op PulseAudio events -> {len(in_window)} commits in {span:.1f}s (clock only)"
    else:
        msg += "; no-op event check skipped (pactl unavailable)"
    common.ok(msg)


if __name__ == "__main__":
    main()
