#!/usr/bin/env python3
"""The clock's year calendar opens, navigates, highlights on hover, and closes.

Added during the Rust port (M4), where the popup machinery was rebuilt with
the calendar as its first user -- before test_popups.py can run (it waits for
the CPU, memory and volume boxes). Covers the clock's part of test_popups.py,
plus year navigation and hover, which no other test does. Checks, on a
test-control instance:

  - a right-click on the clock opens a menu with exactly one click-catcher,
    drawn at the menu's size with a mauve border on a mantle background;
  - the menu surface really gets a buffer on screen: its first paint comes
    before the compositor's first configure and must be redone after it
    (BUG.md "A popup's first paint never happened"). `snapshot` alone can't
    show this -- it draws the menu again into a fresh image -- so it's read
    from the WAYLAND_DEBUG log: an attach + commit on the overlay-layer
    surface;
  - a left-click on the clock does nothing;
  - clicking the header's next chevron changes the picture, clicking the year
    text returns to exactly the original, and the previous chevron gives a
    third, different picture;
  - hovering the next chevron changes the picture, moving away restores it;
  - a catcher click closes it with no catcher left behind;
  - opening it costs little memory, and the catcher is a 1x1 buffer
    stretched with wp_viewporter (full-size catchers were ~8 MB per output).

Click targets come from the calendar's layout constants (14px outer margin,
30px chevron boxes, header row starting at y=14). Passes against both the C
bar and the Rust port (`--bin PATH`).
"""

import hashlib
import re
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402

MAX_POPUP_RSS_MB = 4.0
HEADER_Y = 20      # inside the header row (it starts at y=14)
# The menu is the overlay-layer (3) layer surface in the popup namespace.
_MENU_SURFACE = re.compile(r'get_layer_surface\(new id \w+#\d+, wl_surface#(\d+), wl_output#\d+, 3, "custombar-popup"\)')
CHEVRON_X = 29     # centre of a chevron box: 14px margin + half of 30px


def snapshot(bar, tmp, name):
    """The open menu's pixels, as (width, height, digest of the PNG's pixels)."""
    png = Path(tmp) / f"{name}.png"
    bar.must(f"snapshot {png}")
    w, h, px = common.png_pixels(png)
    digest = hashlib.sha256(repr(px).encode()).hexdigest()
    return w, h, px, digest


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)
    failures = []

    with tempfile.TemporaryDirectory() as tmp:
        log = Path(tmp) / "wl.log"
        with common.ControlledBar(binary, wayland_debug=True, stderr_path=log) as bar:
            bar.wait_ready(("clock",))
            time.sleep(0.5)

            # A left-click on the clock does nothing.
            bar.click_module("clock", "left")
            time.sleep(0.3)
            if bar.must("menu")["open"]:
                failures.append("a left-click on the clock opened a menu")
                bar.must("catcher-click")
                bar.wait_menu(False)

            before = common.proc_memory_mb(bar.proc.pid)["Rss"]
            bar.click_module("clock", "right")
            menu = bar.wait_menu(True)
            time.sleep(0.8)
            grown = common.proc_memory_mb(bar.proc.pid)["Rss"] - before
            # The log so far: opened, but nothing else has asked for a repaint yet.
            opened_log = log.read_text(errors="replace")
            if menu["catchers"] != 1:
                failures.append(f"{menu['catchers']} catchers, expected 1")
            if grown > MAX_POPUP_RSS_MB:
                failures.append(f"opening the calendar grew RSS by {grown:.1f} MB "
                                f"(> {MAX_POPUP_RSS_MB} MB -- full-size catcher buffers?)")

            w, h, px, home = snapshot(bar, tmp, "home")
            if (w, h) != (menu["width"], menu["height"]):
                failures.append(f"snapshot {w}x{h} != surface {menu['width']}x{menu['height']}")
            if not common.close_rgb(px[0][w // 2], common.COLORS["mauve"]):
                failures.append(f"border {px[0][w // 2]} is not mauve")
            if not common.close_rgb(px[5][5], common.COLORS["mantle"]):
                failures.append(f"background {px[5][5]} is not mantle")

            # Year navigation: next, back to this year, previous.
            bar.must(f"menu-click {w - CHEVRON_X} {HEADER_Y}")
            next_year = snapshot(bar, tmp, "next")[3]
            bar.must(f"menu-click {w // 2} {HEADER_Y}")
            back = snapshot(bar, tmp, "back")[3]
            bar.must(f"menu-click {CHEVRON_X} {HEADER_Y}")
            prev_year = snapshot(bar, tmp, "prev")[3]
            if next_year == home:
                failures.append("the next-year chevron didn't change the calendar")
            if back != home:
                failures.append("clicking the year text didn't return to the current year")
            if prev_year in (home, next_year):
                failures.append("the previous-year chevron didn't show a different year")
            bar.must(f"menu-click {w // 2} {HEADER_Y}")  # back to this year

            # Hover: over the next chevron, then away from every target.
            bar.must(f"menu-motion {w - CHEVRON_X} {HEADER_Y}")
            hovered = snapshot(bar, tmp, "hover")[3]
            bar.must(f"menu-motion {w // 2} {h - 5}")
            unhovered = snapshot(bar, tmp, "unhover")[3]
            if hovered == home:
                failures.append("hovering the next chevron didn't highlight it")
            if unhovered != home:
                failures.append("moving away didn't clear the hover highlight")

            bar.must("catcher-click")
            closed = bar.wait_menu(False)
            if closed["catchers"] != 0:
                failures.append(f"{closed['catchers']} catchers left after closing")

        text = log.read_text(errors="replace")
        menus = _MENU_SURFACE.findall(opened_log)
        if not menus:
            failures.append("no overlay-layer popup surface in the log")
        else:
            # The right-click's menu must be on screen before anything else
            # (hover, a year click) paints it again.
            sid = menus[-1]
            attached = re.search(rf"wl_surface#{sid}\.attach\(wl_buffer#\d+", opened_log)
            committed = re.search(rf"wl_surface#{sid}\.attach\(wl_buffer#\d+.*?wl_surface#{sid}\.commit\(\)",
                                  opened_log, re.S)
            if not (attached and committed):
                failures.append(f"the calendar's surface wl_surface#{sid} never got a buffer -- "
                                "its first paint was lost")
        viewports = len(re.findall(r"wp_viewport#\d+\.set_destination\(\d+, \d+\)", text))
        if viewports == 0:
            failures.append("no wp_viewport.set_destination in the log -- catchers aren't using viewporter")

    if failures:
        common.fail("; ".join(failures))
    common.ok(f"calendar {w}x{h}: opens with 1 catcher, navigates years, hover highlights, "
              f"closes; +{grown:.1f} MB RSS; {viewports} viewport-stretched catcher configures")


if __name__ == "__main__":
    main()
