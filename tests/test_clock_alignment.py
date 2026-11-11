#!/usr/bin/env python3
"""Clock ticks must land at the start of each wall-clock second.

Regression test for the clock timer being a CLOCK_MONOTONIC interval counted
from startup, which made the shown seconds lag real time by up to ~1s.

Reads the wall-clock timestamps WAYLAND_DEBUG puts on every request: every
second of the run should have a wl_surface.commit within MAX_LAG_MS of the
second boundary (the clock-only redraw commits right after the tick).

The bar normally hides seconds (one commit a minute), so this runs it with
STICKBAR_TEST_CLOCK_SECONDS to get a per-second change: the timer and its
alignment are the same either way, only the shown text differs.
"""

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402

RUN_S = 8
WARMUP_S = 2        # ignore startup (font loading delays the first frames)
MAX_LAG_MS = 50.0   # measured ~0.1-2ms; generous for a loaded machine
CLOCK_SECONDS = {"STICKBAR_TEST_CLOCK_SECONDS": "1"}


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)

    with tempfile.TemporaryDirectory() as tmp:
        log = Path(tmp) / "wl.log"
        bar = common.start_bar(binary, stderr_path=log, wayland_debug=True, env_extra=CLOCK_SECONDS)
        common.wait(RUN_S)
        common.stop_bar(bar)
        commits = [t for t, _sid, req, _ in common.parse_wayland_log(log) if req == "commit"]

    if not commits:
        common.fail("no wl_surface.commit lines in the WAYLAND_DEBUG log")
    start = commits[0] + WARMUP_S
    # Earliest commit within each whole second after warmup.
    first_in_second = {}
    for t in commits:
        if t < start:
            continue
        sec = int(t)
        first_in_second[sec] = min(first_in_second.get(sec, t), t)
    # Drop the last (possibly partial) second.
    secs = sorted(first_in_second)[:-1]
    if len(secs) < 3:
        common.fail(f"only {len(secs)} full seconds of commits observed")

    lags = [(first_in_second[s] - s) * 1000 for s in secs]
    late = [f"{lag:.1f}ms" for lag in lags if lag > MAX_LAG_MS]
    if late:
        common.fail(f"clock commits not aligned to the second: lags {', '.join(late)} "
                    f"(all: {', '.join(f'{lag:.1f}' for lag in lags)} ms)")
    common.ok(f"{len(lags)} ticks, each within {max(lags):.2f}ms after the second boundary")


if __name__ == "__main__":
    main()
