#!/usr/bin/env python3
"""Stands in for DacController's blackpearl-eq.py in test_eq.py: never touches
the DAC. `profile NAME` logs the name, waits a moment (so the "applying" state
can be seen), then writes the state file the way the real script does -- temp
file + rename, into $XDG_CACHE_HOME/blackpearl-eq/.
"""

import json
import os
import sys
import time

CURVE_POINTS = 64


def write_state(profile, gain="low", preamp=-3.0, enabled=True, boost_db=4.0):
    state_dir = os.path.join(os.environ["XDG_CACHE_HOME"], "blackpearl-eq")
    os.makedirs(state_dir, exist_ok=True)
    freqs = [round(20.0 * 1000.0 ** (i / (CURVE_POINTS - 1)), 1) for i in range(CURVE_POINTS)]
    # A bass shelf: +boost_db on the left of the graph, falling to 0 dB.
    db = [round(boost_db * (1.0 - i / (CURVE_POINTS - 1)), 2) for i in range(CURVE_POINTS)]
    state = {"profile": profile, "gain": gain, "preamp": preamp, "enabled": enabled,
             "curve": {"freqs": freqs, "db": db}, "peak_db": max(db), "min_db": min(db)}
    tmp = os.path.join(state_dir, "state.json.tmp")
    with open(tmp, "w") as fh:
        json.dump(state, fh)
    os.replace(tmp, os.path.join(state_dir, "state.json"))


if __name__ == "__main__":
    if sys.argv[1:2] == ["profile"]:
        with open(os.environ["FAKE_EQ_LOG"], "a") as log:
            log.write(sys.argv[2] + "\n")
        time.sleep(float(os.environ.get("FAKE_EQ_DELAY", "1.0")))
        write_state(sys.argv[2])
