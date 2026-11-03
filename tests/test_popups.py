#!/usr/bin/env python3
"""Every dropdown opens from its module, draws, and closes again.

Driven through the test-control channel (see tests/lib/common.py's
ControlledBar), so no real cursor moves: for each module, injects the click
that opens its dropdown and checks that

  - a popup surface appears, with exactly one click-catcher per output (the
    test instance runs on one output, so one) -- not a leaked extra;
  - its rendered pixels (a PNG of exactly what the bar painted) have the
    module's accent colour as the border and the mantle background inside;
  - a click on the catcher closes it, leaving no popup or catcher behind.

Plus, across the run:
  - the CPU/GPU/memory dropdowns replace each other (they open at one spot);
  - catchers are 1x1 buffers stretched with wp_viewporter, and opening a
    dropdown costs little memory (it used to add ~8 MB per output in
    full-size transparent catcher buffers -- see CLAUDE.md "D-Bus / system
    tray").
"""

import re
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402

# (module, button, accent) -- accents from each module's own box colour.
POPUPS = [
    ("cpu", "right", "peach"),
    ("gpu", "right", "green"),
    ("mem", "right", "yellow"),
    ("network", "right", "teal"),
    ("clock", "right", "mauve"),
    ("volume", "right", "sky"),
    ("volume", "middle", "sky"),  # the per-app mixer
    ("mic", "right", "blue"),
    ("media", "right", "yellow"),  # only if a player is running
]
MAX_POPUP_RSS_MB = 4.0  # a full-size catcher buffer alone was ~8 MB on 1920x1080


def rss_mb(pid):
    return common.proc_memory_mb(pid)["Rss"]


def check_popup(bar, tmp, module, button, accent, failures):
    rects = bar.must(f"rects {bar.output}")["rects"]
    if module not in rects:
        return f"{module}: not shown (skipped)"
    bar.click_module(module, button)
    menu = bar.wait_menu(True)
    label = f"{module}/{button}"
    if menu["catchers"] != 1:
        failures.append(f"{label}: {menu['catchers']} catchers, expected 1")
    png = Path(tmp) / f"{module}-{button}.png"
    bar.must(f"snapshot {png}")
    w, h, px = common.png_pixels(png)
    if (w, h) != (menu["width"], menu["height"]):
        failures.append(f"{label}: snapshot {w}x{h} != surface {menu['width']}x{menu['height']}")
    if not common.close_rgb(px[0][w // 2], common.COLORS[accent]):
        failures.append(f"{label}: border {px[0][w // 2]} is not {accent}")
    if not common.close_rgb(px[5][5], common.COLORS["mantle"]):
        failures.append(f"{label}: background {px[5][5]} is not mantle")
    bar.must("catcher-click")
    closed = bar.wait_menu(False)
    if closed["catchers"] != 0:
        failures.append(f"{label}: {closed['catchers']} catchers left after closing")
    return f"{label} {w}x{h}"


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)
    failures, seen = [], []

    with tempfile.TemporaryDirectory() as tmp:
        log = Path(tmp) / "wl.log"
        with common.ControlledBar(binary, wayland_debug=True, stderr_path=log) as bar:
            bar.wait_ready(("clock", "cpu", "mem", "volume"))
            time.sleep(0.5)
            for module, button, accent in POPUPS:
                seen.append(check_popup(bar, tmp, module, button, accent, failures))

            # The three sysmon dropdowns open at one spot: each replaces the last.
            sizes = {}
            for module in ("cpu", "gpu", "mem"):
                bar.click_module(module)
                sizes[module] = bar.wait_menu(True)
                if sizes[module]["catchers"] != 1:
                    failures.append(f"{module} after another sysmon dropdown: "
                                    f"{sizes[module]['catchers']} catchers (one wasn't closed)")
            bar.must("catcher-click")
            bar.wait_menu(False)

            # Memory cost of an open dropdown (catchers included).
            time.sleep(0.5)
            before = rss_mb(bar.proc.pid)
            bar.click_module("network")
            bar.wait_menu(True)
            time.sleep(0.8)
            grown = rss_mb(bar.proc.pid) - before
            bar.must("catcher-click")
            bar.wait_menu(False)
            if grown > MAX_POPUP_RSS_MB:
                failures.append(f"opening the network dropdown grew RSS by {grown:.1f} MB "
                                f"(> {MAX_POPUP_RSS_MB} MB -- full-size catcher buffers again?)")

        text = log.read_text(errors="replace")
        viewports = len(re.findall(r"wp_viewport#\d+\.set_destination\(\d+, \d+\)", text))
        if viewports == 0:
            failures.append("no wp_viewport.set_destination in the log -- catchers aren't using viewporter")

    if failures:
        common.fail("; ".join(failures))
    common.ok(f"{len(seen)} dropdowns opened, drew and closed ({', '.join(seen)}); "
              f"sysmon dropdowns replace each other; network dropdown +{grown:.1f} MB RSS; "
              f"{viewports} viewport-stretched catcher configures")


if __name__ == "__main__":
    main()
