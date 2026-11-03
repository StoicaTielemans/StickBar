#!/usr/bin/env python3
"""The media dropdown's cards and controls work, against a fake player.

Starts tests/lib/fake_mpris.py (an MPRIS player that only logs the calls it
gets), so no real player is ever paused or skipped, then through the
test-control channel:

  - the bar's MPRIS client sees it with the right title/artist/album/status;
  - the dropdown has a card for it; its play/pause chip sends PlayPause (and
    the status the bar shows follows the player), prev/next send
    Previous/Next;
  - when the player goes away, its card goes and the popup shrinks (or
    closes, if it was the only player) instead of keeping its old size.
"""

import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402

FAKE = Path(__file__).resolve().parent / "lib" / "fake_mpris.py"
APP = "StickbarTestPlayer"
LIFETIME_S = 12


def calls(log):
    return [line.strip() for line in log.read_text().splitlines() if line.strip()]


def wait_for(pred, timeout=3.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = pred()
        if value:
            return value
        time.sleep(0.1)
    return pred()


def fake_player(bar):
    players = bar.must("dump media")["players"]
    return next((p for p in players if p["app"] == APP), None)


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)
    if subprocess.run([sys.executable, "-c", "from gi.repository import Gio"], capture_output=True).returncode:
        common.skip("needs PyGObject (gi) for the fake MPRIS player")
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        log = Path(tmp) / "fake.log"
        log.write_text("")
        fake = subprocess.Popen([sys.executable, str(FAKE), str(log), str(LIFETIME_S)])
        try:
            if not wait_for(lambda: "owned" in calls(log), 5):
                common.fail("fake MPRIS player never got its bus name")
            with common.ControlledBar(binary) as bar:
                bar.wait_ready(("media",))
                player = wait_for(lambda: fake_player(bar))
                if not player:
                    common.fail("the bar never saw the fake player")
                expected = {"title": "Fake Track Title", "artist": "Fake Artist", "album": "Fake Album",
                            "status": "Paused", "length_us": 200_000_000}
                for key, value in expected.items():
                    if player[key] != value:
                        failures.append(f"{key} {player[key]!r}, expected {value!r}")

                bar.click_module("media")
                opened = bar.wait_menu(True)
                players = len(bar.must("dump media")["players"])
                if opened["media_cards"] != players:
                    failures.append(f"{opened['media_cards']} cards for {players} players")

                for button, call in (("play", "PlayPause"), ("next", "Next"), ("prev", "Previous")):
                    t = bar.must(f"target media {button} {APP}")
                    bar.must(f"menu-click {t['x']} {t['y']}")
                    if not wait_for(lambda: call in calls(log)):
                        failures.append(f"{button} chip didn't send {call} (got {calls(log)})")
                if not wait_for(lambda: (p := fake_player(bar)) and p["status"] == "Playing"):
                    failures.append(f"status after PlayPause: {fake_player(bar)}")

                fake.terminate()
                fake.wait(timeout=5)
                if players == 1:
                    bar.wait_menu(False, timeout=4.0)
                    gone = "popup closed (it was the only player)"
                else:
                    after = bar.wait_menu(True, timeout=4.0, media_cards=players - 1)
                    if after["height"] >= opened["height"]:
                        failures.append(f"popup didn't shrink: {opened['height']} -> {after['height']}")
                    gone = f"popup shrank {opened['height']} -> {after['height']}px"
                    bar.must("catcher-click")
        finally:
            if fake.poll() is None:
                fake.kill()
    if failures:
        common.fail("; ".join(failures))
    common.ok(f"fake player's metadata read right; play/next/prev chips sent PlayPause/Next/Previous; "
              f"after it quit: {gone}")


if __name__ == "__main__":
    main()
