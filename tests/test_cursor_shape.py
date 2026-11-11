#!/usr/bin/env python3
"""The pointer turns into a hand over anything clickable, an arrow elsewhere.

A Rust-only addition (docs/porting.md, "Rust-only additions"): the C bar never
set a cursor. The bar names the shape with the wp_cursor_shape_v1 protocol and
the compositor draws it. A test-control instance takes no real pointer input
(its surfaces have an empty input region), so it never sends a shape itself;
instead this asks it, with the `cursor` / `menu-cursor` commands, which shape
it would show at a point -- the exact decision the real pointer path uses.

Checks:
  - the clock box (right-click opens the calendar), the Arch logo (opens
    the power menu) and a workspace pill give a hand; the gap just after the last pill (empty bar) the arrow. The pills
    come from the real Hyprland (read only), so the pill check is skipped if
    the test monitor happens to show none;
  - with the calendar open, its chevrons and its year text give a hand, the
    middle of a month card the arrow.

Skips against the C bar, which has no `cursor` command (and no hand cursor).
"""

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402

HEADER_Y = 20   # inside the calendar's header row
CHEVRON_X = 29  # centre of the left chevron's box (see test_calendar.py)
CARD_Y = 200    # well inside the first row of month cards


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)
    failures = []

    with common.ControlledBar(binary) as bar:
        rects = bar.wait_ready(("clock",))
        time.sleep(0.5)  # the pills are drawn in the same frame, but be generous
        rects = bar.must(f"rects {bar.output}")["rects"]
        if not bar.cmd(f"cursor {bar.output} 0 0").get("ok"):
            common.skip("this bar has no `cursor` command (the C bar: no hand cursor)")
        time.sleep(0.3)

        def shape(command):
            return bar.must(command)["shape"]

        x, y, w, h = rects["clock"]
        ax, _, aw, _ = rects["arch"]
        expect = [
            (f"cursor {bar.output} {x + w / 2:.1f} {y + h / 2:.1f}", "pointer", "the clock box"),
            (f"cursor {bar.output} {ax + aw / 2:.1f} {h / 2:.1f}", "pointer", "the Arch logo"),
        ]
        if "workspaces" in rects:
            wx, _, ww, _ = rects["workspaces"]
            expect += [
                (f"cursor {bar.output} {wx + 5:.1f} {h / 2:.1f}", "pointer", "the first workspace pill"),
                # 3px into the 6px gap after the last pill: no box there.
                (f"cursor {bar.output} {wx + ww + 3:.1f} {h / 2:.1f}", "default", "the gap after the pills"),
            ]
        else:
            # 3px into the gap after the Arch logo: nothing there without pills.
            expect.append((f"cursor {bar.output} {ax + aw + 3:.1f} {h / 2:.1f}", "default", "the gap after the logo"))
        for command, wanted, what in expect:
            got = shape(command)
            if got != wanted:
                failures.append(f"{what}: {got}, expected {wanted}")

        bar.click_module("clock", "right")
        menu = bar.wait_menu(True)
        mw = menu["width"]
        expect = [
            (f"menu-cursor {CHEVRON_X} {HEADER_Y}", "pointer", "the previous-year chevron"),
            (f"menu-cursor {mw - CHEVRON_X} {HEADER_Y}", "pointer", "the next-year chevron"),
            (f"menu-cursor {mw // 2} {HEADER_Y}", "pointer", "the year text"),
            (f"menu-cursor {mw // 8} {CARD_Y}", "default", "a month card"),
        ]
        for command, wanted, what in expect:
            got = shape(command)
            if got != wanted:
                failures.append(f"calendar, {what}: {got}, expected {wanted}")
        bar.must("catcher-click")
        bar.wait_menu(False)

    if failures:
        common.fail("; ".join(failures))
    common.ok("hand over the clock, the Arch logo, the workspace pills and the calendar's targets; "
              "arrow over the empty bar and the cards")


if __name__ == "__main__":
    main()
