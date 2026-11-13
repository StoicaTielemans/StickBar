#!/usr/bin/env python3
"""The EQ card in the media dropdown, and profile switching, work without the DAC.

Points the bar at a scratch XDG_CACHE_HOME and at tests/lib/fake_eq_script.py
via BLACKPEARL_EQ_SCRIPT, so the real blackpearl-eq.py is never run and the
DAC is never touched. Through the test-control channel:

  - the state file is read, and the media dropdown (opened with a fake MPRIS
    player running, since the media box needs a player) has a chip per profile;
  - the active profile's chip has the accent border, the others the grey one,
    and hovering a chip lightens it;
  - a click runs `profile NAME`: the pill turns yellow while it's pending, then
    the script's state write moves the accent border to that chip and the pill
    back to green, all with the popup open and at the same size;
  - a state write from outside, with the EQ off, turns the pill grey;
  - with no state file at all, the EQ card shows the grey pill.

The EQ used to be a box and dropdown of its own; it moved into the media
module (docs/modules/eq.md), and this test with it.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402
import fake_eq_script  # noqa: E402

FAKE = Path(__file__).resolve().parent / "lib" / "fake_eq_script.py"
# A fake MPRIS player, so the media box (the way into the dropdown) is there.
FAKE_PLAYER = Path(__file__).resolve().parent / "lib" / "fake_mpris.py"
PLAYER_LIFETIME_S = 60
# Set once the test has re-run itself under dbus-run-session.
PRIVATE_BUS_VAR = "STICKBAR_TEST_EQ_PRIVATE_BUS"


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

    def at(self, what, dx=0):
        """The pixel at `target eq <what>`, `dx` px to the right."""
        t = self.bar.must(f"target eq {what}")
        _, _, rows = self.pixels()
        return rows[int(t["y"])][int(t["x"]) + dx]

    def border(self, profile):
        return self.at(f"{profile}:edge")

    def fill(self, profile):
        return self.at(f"{profile}:edge", 3)

    def pill(self):
        # Inside the pill's right padding, so it's never a pixel of its text.
        return self.at("pill")


def is_color(pixel, name):
    return common.close_rgb(pixel, common.COLORS[name])


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)
    if subprocess.run([sys.executable, "-c", "from gi.repository import Gio"], capture_output=True).returncode:
        common.skip("needs PyGObject (gi) for the fake MPRIS player")
    # On its own session bus, so the user's real players can't change the
    # dropdown's size mid-test (a track change adds or drops an artist line).
    if not os.environ.get(PRIVATE_BUS_VAR):
        if not shutil.which("dbus-run-session"):
            common.skip("needs dbus-run-session for a private session bus")
        env = dict(os.environ, **{PRIVATE_BUS_VAR: "1"})
        sys.exit(subprocess.run(["dbus-run-session", "--", sys.executable, __file__], env=env).returncode)
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        cache = Path(tmp) / "cache"
        log = Path(tmp) / "fake.log"
        log.write_text("")
        os.environ["XDG_CACHE_HOME"] = str(cache)  # for fake_eq_script.write_state() below
        env = {"XDG_CACHE_HOME": str(cache), "BLACKPEARL_EQ_SCRIPT": str(FAKE), "FAKE_EQ_LOG": str(log)}

        player_log = Path(tmp) / "player.log"
        player = subprocess.Popen([sys.executable, str(FAKE_PLAYER), str(player_log), str(PLAYER_LIFETIME_S)])
        try:
            run(binary, env, tmp, log, failures)
        finally:
            player.kill()
            player.wait()
    if failures:
        common.fail("; ".join(failures))
    common.ok("chips, hover and the pill follow the state; a click ran the fake script and the accent "
              "border moved, with the popup's size unchanged; no state: grey pill")


def open_dropdown(bar):
    """Right-clicks the media box once the fake player is in the bar's list: the
    box is there from the start (idle text), and a player arriving later would
    resize an already open dropdown."""
    if not wait_for(lambda: bar.must("dump media")["players"]):
        common.fail("the bar never saw the fake player")
    bar.click_module("media")
    return bar.wait_menu(True)


def run(binary, env, tmp, log, failures):
    """Both bars: without a state file, then with one."""
    # No state file yet.
    with common.ControlledBar(binary, env_extra=env) as bar:
        if bar.must("dump eq")["available"]:
            failures.append("state available in an empty cache directory")
        opened = open_dropdown(bar)
        if opened["eq_cards"] != 5:
            failures.append(f"{opened['eq_cards']} profile chips without state, expected 5")
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

        opened = open_dropdown(bar)
        if opened["eq_cards"] != 5:
            failures.append(f"{opened['eq_cards']} profile chips, expected 5")
        if not is_color(popup.border("zero2"), "lavender"):
            failures.append(f"active chip's border is {popup.border('zero2')}")
        if not is_color(popup.border("dt990"), "surface1"):
            failures.append(f"inactive chip's border is {popup.border('dt990')}")
        if not is_color(popup.fill("dt990"), "base"):
            failures.append(f"unhovered chip's fill is {popup.fill('dt990')}")
        if not is_color(popup.pill(), "green"):
            failures.append(f"enabled pill is {popup.pill()}, expected green")

        t = bar.must("target eq dt990")
        bar.must(f"menu-motion {t['x']} {t['y']}")
        if not is_color(popup.fill("dt990"), "surface0"):
            failures.append(f"hovered chip's fill is {popup.fill('dt990')}")
        bar.must("menu-motion 1 1")
        if not is_color(popup.fill("dt990"), "base"):
            failures.append("chip stayed lit after the pointer left it")

        # Clicks outside the chips do nothing (here: on the EQ card's pill).
        p = bar.must("target eq pill")
        bar.must(f"menu-click {p['x']} {p['y']}")
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


if __name__ == "__main__":
    main()
