#!/usr/bin/env python3
"""Dropdown graphs reach the right border of their card.

The braille graphs used to be a fixed 30 cells (5 minutes) centred in a card
that's always wider (its width comes from the text rows), so the newest
samples -- drawn at the graph's right end -- stopped well short of the card's
right border (~70px on the memory card; the user reported it). Now each
graph fills its card.

A freshly started bar has only a sample or two of history, so the dots sit
only at the graph's right end: in a PNG of the open dropdown (rendered by the
bar itself through the test-control channel), find the graph card, find the
rows whose only drawn pixels are in the card's right part (dot rows -- label
rows have text on the left too), and check the rightmost dot is within a few
pixels of the card's right border.

Checked on graphs that have dots from the very first sample: memory used %
(never 0) and the CPU/GPU temperature graphs (above their 30 °C zero).
CPU/GPU usage and network rates read 0 on the first poll, so they don't.
"""

import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402

MAX_GAP_PX = 14  # card padding (8) + half a cell gap, with a little slack
BACKGROUNDS = ("base", "surface1", "mantle")


def is_background(p):
    return any(common.close_rgb(p, common.COLORS[c], tol=6) for c in BACKGROUNDS)


def graph_gap(png):
    """(gap from rightmost dot to the card's inner right edge, rows checked),
    or None if no dot rows were found."""
    w, h, px = common.png_pixels(png)
    # The graph card: the first run of rows whose middle pixel is `base`.
    mid = w // 2
    card_rows = [y for y in range(h) if common.close_rgb(px[y][mid], common.COLORS["base"], tol=4)]
    if not card_rows:
        return None
    top = card_rows[0]
    rows = [y for y in range(top, h) if common.close_rgb(px[y][mid], common.COLORS["base"], tol=4)
            or not is_background(px[y][mid])]
    # The card's right border: scanning left from the edge on the card's top
    # row-ish, the first surface1 pixel.
    border_x = next((x for x in range(w - 1, mid, -1)
                     if common.close_rgb(px[top + 4][x], common.COLORS["surface1"], tol=6)), None)
    left_x = next((x for x in range(0, mid)
                   if common.close_rgb(px[top + 4][x], common.COLORS["surface1"], tol=6)), None)
    if border_x is None or left_x is None:
        return None
    split = left_x + (border_x - left_x) * 0.6
    rightmost, dot_rows = None, 0
    for y in rows:
        drawn = [x for x in range(left_x + 1, border_x) if not is_background(px[y][x])]
        if drawn and min(drawn) > split:
            dot_rows += 1
            rightmost = max(drawn) if rightmost is None else max(rightmost, max(drawn))
    if rightmost is None:
        return None
    return border_x - rightmost, dot_rows


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)
    results, failures = [], []
    with tempfile.TemporaryDirectory() as tmp, common.ControlledBar(binary) as bar:
        bar.wait_ready(("cpu", "gpu", "mem"))
        time.sleep(0.5)
        for module in ("mem", "cpu", "gpu"):
            bar.click_module(module)
            bar.wait_menu(True)
            png = Path(tmp) / f"{module}.png"
            bar.must(f"snapshot {png}")
            bar.must("catcher-click")
            bar.wait_menu(False)
            measured = graph_gap(png)
            if measured is None:
                if module == "gpu" and not bar.must("dump gpu")["available"]:
                    continue  # no NVIDIA GPU: the dropdown has no graph
                failures.append(f"{module}: no graph dots found in the card")
                continue
            gap, rows = measured
            results.append(f"{module} {gap}px ({rows} dot rows)")
            if gap > MAX_GAP_PX:
                failures.append(f"{module}: newest dots end {gap}px before the card border (> {MAX_GAP_PX})")
    if failures:
        common.fail("; ".join(failures))
    common.ok("graphs reach their card's right border: " + ", ".join(results))


if __name__ == "__main__":
    main()
