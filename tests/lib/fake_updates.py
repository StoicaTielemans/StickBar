#!/usr/bin/env python3
"""Stands in for both `checkupdates` and `yay -Qua` in test_updates.py, so the
test never touches pacman, the AUR or the network. The bar runs it through
STICKBAR_TEST_UPDATES_REPO / _AUR (`yay` gets `-Qua`, which is how it tells
which one it is).

What it answers comes from the JSON file at $FAKE_UPDATES_STATE:
{"repo": N | "none" | "fail", "aur": N | "none" | "fail"}
  N      -> N `name old -> new` lines, exit 0
  "none" -> no output, exit 2 (checkupdates) or 1 (yay), as the real ones do
  "fail" -> exit 1 (checkupdates) or 3 (yay) after a message on stderr

Every run also appends its SigBlk line from /proc/self/status to
$FAKE_UPDATES_LOG, so the test can check the bar started it with an empty
signal mask.
"""

import json
import os
import sys

if __name__ == "__main__":
    aur = sys.argv[1:2] == ["-Qua"]
    with open(os.environ["FAKE_UPDATES_LOG"], "a") as log:
        sigblk = next(l for l in open("/proc/self/status") if l.startswith("SigBlk:"))
        log.write(("aur " if aur else "repo ") + sigblk.split()[1] + "\n")
    with open(os.environ["FAKE_UPDATES_STATE"]) as fh:
        answer = json.load(fh)["aur" if aur else "repo"]
    if answer == "none":
        sys.exit(1 if aur else 2)
    if answer == "fail":
        print("fake: failing on purpose", file=sys.stderr)
        sys.exit(3 if aur else 1)
    prefix = "aurpkg" if aur else "pkg"
    for i in range(answer):
        print(f"{prefix}{i:02d} 1.{i}-1 -> 1.{i}-2")
