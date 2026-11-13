#!/usr/bin/env python3
"""The pending-updates box and dropdown follow the update check, without
pacman, the AUR or the network.

Points the bar at tests/lib/fake_updates.py for both `checkupdates` and
`yay -Qua` (STICKBAR_TEST_UPDATES_REPO / _AUR). A test-mode bar never checks
by itself, so each check here is the test's `refresh updates`:

  - no box before any check (nothing known yet);
  - 40 repo + 2 AUR updates: the box appears, `dump updates` lists exactly
    the fake's lines, and the right-click dropdown shows 30 package rows (the
    cap) with the AUR ones kept;
  - the repo check failing: the last list is kept, the error is reported, and
    the open dropdown grows an error line instead of emptying;
  - nothing to update: the box disappears and the open dropdown closes;
  - every run of the fake got an empty signal mask (SigBlk 0), the rule for
    any child of the bar (docs/modules/eq.md).
"""

import json
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402

FAKE = Path(__file__).resolve().parent / "lib" / "fake_updates.py"
MAX_ROWS = 30  # MAX_ROWS in src/modules/updates/popup.rs


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        state = Path(tmp) / "state.json"
        log = Path(tmp) / "runs.log"
        log.write_text("")
        env = {"STICKBAR_TEST_UPDATES_REPO": str(FAKE), "STICKBAR_TEST_UPDATES_AUR": str(FAKE),
               "FAKE_UPDATES_STATE": str(state), "FAKE_UPDATES_LOG": str(log)}

        def answer(repo, aur):
            state.write_text(json.dumps({"repo": repo, "aur": aur}))

        with common.ControlledBar(binary, env_extra=env) as bar:
            def check():
                bar.must("refresh updates")
                deadline = time.time() + 10
                while time.time() < deadline:
                    d = bar.must("dump updates")
                    if not d["checking"]:
                        return d
                    time.sleep(0.05)
                common.fail("the update check never finished")

            def has_box():
                time.sleep(0.3)  # the redraw after the result
                return "updates" in bar.must(f"rects {bar.output}")["rects"]

            bar.wait_ready()
            if has_box():
                failures.append("an updates box before any check")

            answer(40, 2)
            d = check()
            expected_repo = [f"pkg{i:02d} 1.{i}-1 -> 1.{i}-2" for i in range(40)]
            expected_aur = [f"aurpkg{i:02d} 1.{i}-1 -> 1.{i}-2" for i in range(2)]
            if (d["repo"], d["aur"], d["total"]) != (expected_repo, expected_aur, 42):
                failures.append(f"dump after 40+2: total {d['total']}, repo[:2] {d['repo'][:2]}, aur {d['aur']}")
            if not has_box():
                failures.append("no box with 42 updates")
            bar.click_module("updates")
            opened = bar.wait_menu(True)
            if opened["update_rows"] != MAX_ROWS:
                failures.append(f"{opened['update_rows']} rows shown, expected the cap of {MAX_ROWS}")

            answer("fail", 2)
            d = check()
            if d["total"] != 42 or "checkupdates failed" not in d["last_error"]:
                failures.append(f"after a failed check: total {d['total']}, error {d['last_error']!r}")
            after_fail = bar.wait_menu(True)
            if after_fail["height"] <= opened["height"]:
                failures.append(f"no error line: height {opened['height']} -> {after_fail['height']}")

            answer("none", "none")
            d = check()
            if d["total"] != 0 or d["last_error"]:
                failures.append(f"after 'none': total {d['total']}, error {d['last_error']!r}")
            if has_box():
                failures.append("the box stayed with nothing to update")
            bar.wait_menu(False)

        runs = log.read_text().split("\n")
        masks = {line.split()[1] for line in runs if line.strip()}
        if len([r for r in runs if r.strip()]) != 6:
            failures.append(f"{len(runs)} fake runs logged, expected 6: {runs}")
        if masks != {"0000000000000000"}:
            failures.append(f"a check ran with blocked signals: SigBlk {masks}")

    if failures:
        common.fail("; ".join(failures))
    common.ok("no box before a check; 40+2 updates listed exactly, dropdown capped at 30 rows; "
              "a failed check kept the list and showed the error; 'none' hid the box and closed "
              "the dropdown; every check ran with SigBlk 0")


if __name__ == "__main__":
    main()
