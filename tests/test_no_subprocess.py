#!/usr/bin/env python3
"""The bar must not spawn any subprocess during normal running.

Regression test for the GPU module's old popen("nvidia-smi ...") every 5s,
now replaced by in-process NVML. Polls the bar's /proc children every few ms
across a couple of 5s sysmon poll ticks. (Only user-triggered actions spawn
anything: EQ profile switching and the power menu -- not exercised here.)
"""

import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402

RUN_S = 12  # covers at least two 5s sysmon polls
POLL_INTERVAL_S = 0.003  # nvidia-smi lived ~15-45ms; 3ms sampling can't miss it


def children(pid):
    found = set()
    for tid in os.listdir(f"/proc/{pid}/task"):
        try:
            for child in open(f"/proc/{pid}/task/{tid}/children").read().split():
                try:
                    found.add((child, open(f"/proc/{child}/comm").read().strip()))
                except OSError:
                    found.add((child, "?"))
        except OSError:
            pass
    return found


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)

    bar = common.start_bar(binary)
    seen = set()
    try:
        end = time.monotonic() + RUN_S
        while time.monotonic() < end:
            if bar.poll() is not None:
                common.fail(f"bar exited early (status {bar.returncode})")
            seen |= children(bar.pid)
            time.sleep(POLL_INTERVAL_S)
    finally:
        common.stop_bar(bar)

    if seen:
        names = sorted({name for _, name in seen})
        common.fail(f"bar spawned {len(seen)} child process(es) in {RUN_S}s: {', '.join(names)}")
    common.ok(f"no child processes spawned in {RUN_S}s")


if __name__ == "__main__":
    main()
