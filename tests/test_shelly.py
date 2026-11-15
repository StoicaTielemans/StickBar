#!/usr/bin/env python3
"""The smart-plug card in the media dropdown, against fake Shelly plugs.

Never touches a real plug: every bar a test starts reads only
STICKBAR_TEST_SHELLY_CONF (common.NO_SHELLY points it at nothing), and here
at a file listing tests/lib/fake_shelly.py's fakes on localhost. Through the
test-control channel:

  - without a settings file there's no card (shelly_rows 0);
  - with one, nothing is asked while the dropdown is closed;
  - opening it reads every plug: on with its watts, off, offline (a port
    nobody listens on) and "password set" (a fake answering 401);
  - it keeps polling while open (every 2 s) and stops once it closes;
  - a row click switches that plug to the opposite (a Set, never a Toggle),
    the badge follows, and only known, reachable rows get the hand cursor;
  - All off / All on switch every plug;
  - the dropdown never changes size for any of it, and draws (snapshot).
"""

import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402
import fake_shelly  # noqa: E402


def wait_for(pred, timeout=4.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = pred()
        if value:
            return value
        time.sleep(0.05)
    return pred()


def plugs(bar):
    return {p["name"]: p for p in bar.must("dump shelly")["plugs"]}


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)
    failures = []

    # No settings file (the default for every test bar): no card.
    with common.ControlledBar(binary) as bar:
        if bar.must("dump shelly")["enabled"]:
            failures.append("a card without a settings file")
        bar.click_module("media")
        if bar.wait_menu(True)["shelly_rows"] != 0:
            failures.append("plug rows without a settings file")
        bar.must("catcher-click")
        bar.wait_menu(False)

    desk = fake_shelly.FakePlug(on=True, watts=42.5)
    tv = fake_shelly.FakePlug(on=False, watts=80.0)
    locked = fake_shelly.FakePlug(password=True)
    live = (desk, tv)
    with tempfile.TemporaryDirectory() as tmp:
        conf = Path(tmp) / "shelly.conf"
        conf.write_text(f"# fakes\ndesk = {desk.addr}\ntv = {tv.addr}\n"
                        f"gone = {fake_shelly.dead_addr()}\nlocked = {locked.addr}\n")
        try:
            with common.ControlledBar(binary, env_extra={"STICKBAR_TEST_SHELLY_CONF": str(conf)}) as bar:
                run(bar, tmp, (desk, tv, locked, live), failures)
        finally:
            for fake in (desk, tv, locked):
                fake.close()
    if failures:
        common.fail("; ".join(failures))
    common.ok("no file: no card; closed: no requests; open: on+watts/off/offline/password read, "
              "polled every 2 s; row clicks and All on/off switched the fakes; size unchanged")


def run(bar, tmp, fakes, failures):
    desk, tv, locked, live = fakes
    state = bar.must("dump shelly")
    if not state["enabled"] or [p["name"] for p in state["plugs"]] != ["desk", "tv", "gone", "locked"]:
        failures.append(f"plugs read wrong: {state}")
    bar.wait_ready(("media",))
    time.sleep(2.5)
    if desk.count() or tv.count():
        failures.append(f"asked while closed: {desk.requests + tv.requests}")

    bar.click_module("media")
    opened = bar.wait_menu(True)
    if opened["shelly_rows"] != 4:
        failures.append(f"{opened['shelly_rows']} plug rows, expected 4")
    if not wait_for(lambda: plugs(bar)["locked"]["state"] != "unknown"):
        failures.append("the first poll never finished")
    got = plugs(bar)
    want = {"desk": ("on", 42.5), "tv": ("off", 0.0), "gone": ("offline", None), "locked": ("error", None)}
    for name, (st, watts) in want.items():
        if (got[name]["state"], got[name]["watts"]) != (st, watts):
            failures.append(f"{name}: {got[name]}, expected {st} {watts}")
    if got["locked"]["error"] != "password set":
        failures.append(f"401 shown as {got['locked']['error']!r}")

    # Polled while open: a couple more requests within ~3 s.
    before = desk.count()
    if not wait_for(lambda: desk.count() >= before + 1, timeout=3.5):
        failures.append("no poll within 3.5 s of the dropdown being open")

    # The hand only over rows a click would switch, and the chips.
    def shape(what):
        t = bar.must(f"target shelly {what}")
        return bar.must(f"menu-cursor {t['x']} {t['y']}")["shape"]
    for what, expect in (("tv", "pointer"), ("gone", "default"), ("locked", "default"),
                         ("all-off", "pointer")):
        if shape(what) != expect:
            failures.append(f"cursor over {what}: {shape(what)}, expected {expect}")

    # A row switches to the opposite of what it shows.
    t = bar.must("target shelly tv")
    bar.must(f"menu-click {t['x']} {t['y']}")
    if not wait_for(lambda: plugs(bar)["tv"]["state"] == "on" and not plugs(bar)["tv"]["pending"]):
        failures.append(f"tv row click: {plugs(bar)['tv']}")
    if tv.sets() != ["true"]:
        failures.append(f"tv got sets {tv.sets()}, expected ['true']")
    if plugs(bar)["tv"]["watts"] != 80.0:
        failures.append(f"tv's watts after switching on: {plugs(bar)['tv']['watts']}")
    t = bar.must("target shelly desk")
    bar.must(f"menu-click {t['x']} {t['y']}")
    if not wait_for(lambda: plugs(bar)["desk"]["state"] == "off"):
        failures.append(f"desk row click: {plugs(bar)['desk']}")
    # An offline row does nothing.
    t = bar.must("target shelly gone")
    bar.must(f"menu-click {t['x']} {t['y']}")
    if plugs(bar)["gone"]["pending"]:
        failures.append("an offline row reacted to a click")

    for chip, word, st in (("all-off", "false", "off"), ("all-on", "true", "on")):
        t = bar.must(f"target shelly {chip}")
        bar.must(f"menu-click {t['x']} {t['y']}")
        if not wait_for(lambda: all(plugs(bar)[n]["state"] == st and not plugs(bar)[n]["pending"]
                                    for n in ("desk", "tv"))):
            failures.append(f"{chip}: {plugs(bar)}")
        if any(f.sets()[-1:] != [word] for f in live):
            failures.append(f"{chip}: last sets {[f.sets()[-1:] for f in live]}")
    if not locked.sets():
        failures.append("All on/off skipped the password plug (it should try, and fail)")

    after = bar.must("menu")
    if (after["width"], after["height"]) != (opened["width"], opened["height"]):
        failures.append(f"popup resized: {opened['width']}x{opened['height']} -> "
                        f"{after['width']}x{after['height']}")
    bar.must(f"snapshot {Path(tmp) / 'shelly.png'}")

    bar.must("catcher-click")
    bar.wait_menu(False)
    time.sleep(0.5)  # a poll in flight as it closed
    closed = desk.count()
    time.sleep(3.0)
    if desk.count() != closed:
        failures.append(f"still polled after closing: {desk.requests[closed:]}")


if __name__ == "__main__":
    main()
