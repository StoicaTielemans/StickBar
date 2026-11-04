#!/usr/bin/env python3
"""The EQ dropdown's cards and profile switching work, without the DAC.

Points the bar at a scratch XDG_CACHE_HOME and at tests/lib/fake_eq_script.py
via BLACKPEARL_EQ_SCRIPT, so the real blackpearl-eq.py is never run and the
DAC is never touched. Through the test-control channel:

  - the state file is read, and the dropdown has a card per profile;
  - the active profile's card has the accent border, the others the grey one,
    and hovering a card lightens it;
  - a click runs `profile NAME`: the pill turns yellow while it's pending, then
    the script's state write moves the accent border to that card and the pill
    back to green, all with the popup open and at the same size;
  - a state write from outside, with the EQ off, turns the pill grey;
  - with no state file at all, the dropdown still opens, with the grey pill.
"""

import os
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402
import fake_eq_script  # noqa: E402

FAKE = Path(__file__).resolve().parent / "lib" / "fake_eq_script.py"
# OUTER_MARGIN in eq_popup.c: the x of a card's left border and of the header's left edge.
CARD_X = 12
HEADER_MID_Y = 23  # OUTER_MARGIN + HEADER_H / 2


def wait_for(pred, timeout=4.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = pred()
        if value:
            return value
        time.sleep(0.05)
    return pred()


class Popup:
    def __init__(self, bar, tmp):
        self.bar, self.png = bar, str(Path(tmp) / "eq.png")

    def pixels(self):
        self.bar.must(f"snapshot {self.png}")
        return common.png_pixels(self.png)

    def card_y(self, profile):
        return int(self.bar.must(f"target eq {profile}")["y"])

    def border(self, profile):
        _, _, rows = self.pixels()
        return rows[self.card_y(profile)][CARD_X]

    def fill(self, profile):
        _, _, rows = self.pixels()
        return rows[self.card_y(profile)][CARD_X + 3]

    def pill(self):
        # Inside the pill's right padding, so it's never a pixel of its text.
        width, _, rows = self.pixels()
        return rows[HEADER_MID_Y][width - CARD_X - 3]


def is_color(pixel, name):
    return common.close_rgb(pixel, common.COLORS[name])


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        cache = Path(tmp) / "cache"
        log = Path(tmp) / "fake.log"
        log.write_text("")
        os.environ["XDG_CACHE_HOME"] = str(cache)  # for fake_eq_script.write_state() below
        env = {"XDG_CACHE_HOME": str(cache), "BLACKPEARL_EQ_SCRIPT": str(FAKE), "FAKE_EQ_LOG": str(log)}

        # No state file yet.
        with common.ControlledBar(binary, env_extra=env) as bar:
            bar.click_module("eq")
            opened = bar.wait_menu(True)
            if opened["eq_cards"] != 5:
                failures.append(f"{opened['eq_cards']} profile cards without state, expected 5")
            if bar.must("dump eq")["available"]:
                failures.append("state available in an empty cache directory")
            if not is_color(Popup(bar, tmp).pill(), "overlay1"):
                failures.append("no-state pill isn't grey")
            bar.must("catcher-click")
            bar.wait_menu(False)

        fake_eq_script.write_state("zero2", gain="low", preamp=-3.0)
        with common.ControlledBar(binary, env_extra=env) as bar:
            popup = Popup(bar, tmp)
            state = bar.must("dump eq")
            if (state["profile"], state["gain"], state["preamp"]) != ("zero2", "low", -3.0):
                failures.append(f"state read wrong: {state}")
            if state["curve_points"] != fake_eq_script.CURVE_POINTS:
                failures.append(f"{state['curve_points']} curve points")

            bar.click_module("eq")
            opened = bar.wait_menu(True)
            if not is_color(popup.border("zero2"), "lavender"):
                failures.append(f"active card's border is {popup.border('zero2')}")
            if not is_color(popup.border("dt990"), "surface1"):
                failures.append(f"inactive card's border is {popup.border('dt990')}")
            if not is_color(popup.fill("dt990"), "base"):
                failures.append(f"unhovered card's fill is {popup.fill('dt990')}")
            if not is_color(popup.pill(), "green"):
                failures.append(f"enabled pill is {popup.pill()}, expected green")

            t = bar.must("target eq dt990")
            bar.must(f"menu-motion {t['x']} {t['y']}")
            if not is_color(popup.fill("dt990"), "surface0"):
                failures.append(f"hovered card's fill is {popup.fill('dt990')}")
            bar.must("menu-motion 1 1")
            if not is_color(popup.fill("dt990"), "base"):
                failures.append("card stayed lit after the pointer left it")

            # Clicks outside the cards do nothing.
            bar.must(f"menu-click {CARD_X + 20} {HEADER_MID_Y}")
            if log.read_text().strip():
                failures.append(f"a header click ran the script: {log.read_text()!r}")

            bar.must(f"menu-click {t['x']} {t['y']}")
            if bar.must("dump eq")["pending"] != "dt990":
                failures.append("click didn't mark dt990 pending")
            if not is_color(popup.pill(), "yellow"):
                failures.append(f"pending pill is {popup.pill()}, expected yellow")
            if not wait_for(lambda: log.read_text().split() == ["dt990"]):
                failures.append(f"script log: {log.read_text()!r}")
            if not wait_for(lambda: bar.must("dump eq")["profile"] == "dt990"):
                failures.append("the script's state write never arrived")
            after = bar.wait_menu(True)
            if (after["width"], after["height"]) != (opened["width"], opened["height"]):
                failures.append(f"popup resized: {opened} -> {after}")
            if not is_color(popup.border("dt990"), "lavender"):
                failures.append("accent border didn't move to the new profile")
            if not is_color(popup.border("zero2"), "surface1"):
                failures.append("the old profile kept its accent border")
            if not is_color(popup.pill(), "green"):
                failures.append("pill didn't return to green after the state write")

            fake_eq_script.write_state("dt990", enabled=False)
            if not wait_for(lambda: not bar.must("dump eq")["enabled"]):
                failures.append("EQ-off state write never arrived")
            if not is_color(popup.pill(), "overlay1"):
                failures.append(f"disabled pill is {popup.pill()}, expected grey")

            bar.must("catcher-click")
            bar.wait_menu(False)
    if failures:
        common.fail("; ".join(failures))
    common.ok("cards, hover and the pill follow the state; a click ran the fake script and the accent "
              "border moved, with the popup's size unchanged; no-state case opens")


if __name__ == "__main__":
    main()
