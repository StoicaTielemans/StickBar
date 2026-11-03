#!/usr/bin/env python3
"""The tray icon's DBusMenu dropdown, against a fake tray app.

Starts tests/lib/fake_tray.py (a StatusNotifierItem whose menu only logs the
clicks it gets -- so no real app's menu is ever triggered), then through the
test-control channel:

  - the fake's icon appears in the tray; a right-click on it opens the menu
    with every entry, drawn on the popup's grey border;
  - labels have their access-key underscores stripped ("_Quit" -> "Quit"),
    and checkmark/radio state and the submenu marker are read;
  - the three separators turn into four cards (checked in the PNG: four
    separate runs of card background down the middle of the popup);
  - clicking a disabled entry does nothing and keeps the menu open;
  - clicking an entry sends its "clicked" event with the right id and closes
    the menu.
"""

import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402

FAKE = Path(__file__).resolve().parent / "lib" / "fake_tray.py"
TITLE = "StickbarTestTray"
LIFETIME_S = 20
EXPECTED = [
    ("Open Window", "", False, False), (None, None, None, None),
    ("Show Notifications", "checkmark", True, False), ("Mode A", "radio", True, False),
    ("Mode B", "radio", False, False), (None, None, None, None),
    ("Disabled Item", "", False, False), ("More", "", False, True), (None, None, None, None),
    ("Quit", "", False, False),
]


def log_lines(log):
    return [line.strip() for line in log.read_text().splitlines() if line.strip()]


def wait_for(pred, timeout=4.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = pred()
        if value:
            return value
        time.sleep(0.1)
    return pred()


def count_cards(png):
    """Runs of `base` (card background) down the popup's left inner column."""
    w, h, px = common.png_pixels(png)
    x = 18  # inside the cards, left of any text
    runs, inside = 0, False
    for y in range(h):
        is_card = common.close_rgb(px[y][x], common.COLORS["base"], tol=4) or \
            common.close_rgb(px[y][x], common.COLORS["surface0"], tol=4)
        if is_card and not inside:
            runs += 1
        inside = is_card
    return runs, px[0][w // 2]


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)
    if subprocess.run([sys.executable, "-c", "from gi.repository import Gio"], capture_output=True).returncode:
        common.skip("needs PyGObject (gi) for the fake tray app")
    failures = []
    with tempfile.TemporaryDirectory() as tmp, common.ControlledBar(binary) as bar:
        # The bar first: if no other StatusNotifierWatcher is running (the
        # user's own bar usually is one), this instance claims the role
        # itself, so the fake always has something to register with.
        bar.wait_ready(("clock",))
        time.sleep(0.5)
        log = Path(tmp) / "fake.log"
        log.write_text("")
        fake = subprocess.Popen([sys.executable, str(FAKE), str(log), str(LIFETIME_S)])
        try:
            if not wait_for(lambda: "registered" in log_lines(log), 5):
                common.fail("fake tray app couldn't register with a StatusNotifierWatcher")
            icon = wait_for(lambda: (r := bar.cmd(f"target trayicon {bar.output} {TITLE}")).get("ok") and r)
            if not icon:
                common.fail("the fake tray icon never appeared in the bar")
            bar.must(f"click {bar.output} {icon['x']} {icon['y']} right")
            menu = bar.wait_menu(True)

            got = [(None, None, None, None) if e["separator"] else
                   (e["label"], e["toggle"], e["toggled"], e["submenu"]) for e in menu["tray_entries"]]
            if got != EXPECTED:
                failures.append(f"entries as shown: {got}")
            png = Path(tmp) / "tray.png"
            bar.must(f"snapshot {png}")
            cards, border = count_cards(png)
            if cards != 4:
                failures.append(f"{cards} cards for 3 separators (expected 4)")
            if not common.close_rgb(border, common.hex_rgb("6c7086")):
                failures.append(f"border {border} is not the neutral overlay0 grey")

            t = bar.must("target traymenu Disabled Item")
            bar.must(f"menu-click {t['x']} {t['y']}")
            time.sleep(0.4)
            if not bar.must("menu")["open"] or any(line.startswith("event") for line in log_lines(log)):
                failures.append("clicking the disabled entry did something")

            t = bar.must("target traymenu Show Notifications")
            bar.must(f"menu-click {t['x']} {t['y']}")
            if not wait_for(lambda: "event 3 Show _Notifications" in log_lines(log)):
                failures.append(f"clicking an entry didn't send its event: {log_lines(log)}")
            bar.wait_menu(False)
        finally:
            fake.kill()
            fake.wait()
    if failures:
        common.fail("; ".join(failures))
    common.ok("fake tray menu: labels/toggles/submenu read right, 3 separators -> 4 cards, grey border, "
              "disabled entry inert, a click sends its event (id 3) and closes the menu")


if __name__ == "__main__":
    main()
