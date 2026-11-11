#!/usr/bin/env python3
"""The workspace pills follow Hyprland, and a click focuses a workspace.

Runs a test-control instance against tests/lib/fake_hyprland.py instead of
the real compositor's IPC sockets, so nothing here can switch the user's
actual workspaces. The fake puts its workspaces in the test monitor's own
5-wide range (index N shows N*5+1 .. N*5+5). Checks:

  - occupied workspaces get pills: a `workspaces` box appears;
  - a workspace event (`openwindow>>`) makes the bar re-query and grow a pill,
    while an unrelated event (`activewindow>>`) causes no re-query at all;
  - a left click on the first pill sends exactly
    `dispatch hl.dsp.focus({ workspace = N })` -- no quotes (BUG.md "Quoted
    dispatch rejected"); a right click sends nothing;
  - when the Lua form is rejected, the click falls back to
    `dispatch workspace N`, and the next click goes straight to the classic
    form (the working style is cached);
  - when the event connection drops, the bar reconnects (once), and events on
    the new connection still update the pills.

Passes against both the C bar and the Rust port (`--bin PATH`).
"""

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402
from fake_hyprland import FakeHyprland  # noqa: E402

SETTLE_S = 0.6


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)
    failures = []

    fake = FakeHyprland()
    try:
        output = common.pick_test_output()
        env = {"HYPRLAND_INSTANCE_SIGNATURE": fake.signature}
        with common.ControlledBar(binary, output=output, env_extra=env) as bar:
            # The bar's index is its own binding order; read it back rather than assume.
            index = bar.must("outputs")["outputs"][0]["index"]
            base = index * 5 + 1
            fake.workspaces = {base: 2, base + 2: 0}
            fake.active = base + 2
            # The bar asked before the fake had state: make it ask again. Only
            # once it has connected to the event socket -- an event pushed
            # before that goes nowhere (this raced and failed once).
            if not fake.wait_for(lambda: fake.event_connections >= 1):
                common.fail("the bar never connected to the event socket")
            fake.send_event(f"workspace>>{base + 2}")
            rects = bar.wait_ready(("clock", "workspaces"))
            two = rects["workspaces"]

            # A workspace event re-queries; the new pill widens the box.
            queries_before = len(fake.commands_matching("j/workspaces"))
            fake.workspaces[base + 1] = 1
            fake.send_event(f"openwindow>>abc,{base + 1},kitty,kitty")
            if not fake.wait_for(lambda: len(fake.commands_matching("j/workspaces")) > queries_before):
                failures.append("openwindow event didn't trigger a re-query")
            time.sleep(SETTLE_S)
            three = bar.must(f"rects {bar.output}")["rects"]["workspaces"]
            if not three[2] > two[2]:
                failures.append(f"workspaces box didn't grow for a new pill ({two[2]} -> {three[2]})")

            # An unrelated event: no re-query.
            queries_before = len(fake.commands_matching("j/workspaces"))
            fake.send_event("activewindow>>kitty,some title")
            time.sleep(SETTLE_S)
            if len(fake.commands_matching("j/workspaces")) != queries_before:
                failures.append("activewindow event triggered a re-query")

            # A left click on the first pill focuses it, with the unquoted Lua form.
            x, y, w, h = three
            lua = f"dispatch hl.dsp.focus({{ workspace = {base} }})"
            classic = f"dispatch workspace {base}"
            bar.must(f"click {bar.output} {x + 5:.1f} {y + h / 2:.1f} left")
            fake.wait_for(lambda: fake.commands_matching("dispatch"))
            sent = fake.commands_matching("dispatch")
            if sent != [lua]:
                failures.append(f"first click sent {sent}, expected [{lua!r}]")

            # A right click sends nothing.
            bar.must(f"click {bar.output} {x + 5:.1f} {y + h / 2:.1f} right")
            time.sleep(SETTLE_S)
            if len(fake.commands_matching("dispatch")) != 1:
                failures.append(f"right click dispatched: {fake.commands_matching('dispatch')[1:]}")

            # The Lua form rejected: fall back to classic, then stay on classic.
            # (A fresh bar would be needed to test the first-ever probe; this one
            # has already cached "Lua works", so it sends Lua once more, gets the
            # error, and switches.)
            fake.reject_lua = True
            bar.must(f"click {bar.output} {x + 5:.1f} {y + h / 2:.1f} left")
            fake.wait_for(lambda: len(fake.commands_matching("dispatch")) >= 3)
            sent = fake.commands_matching("dispatch")[1:]
            if sent != [lua, classic]:
                failures.append(f"after a Lua rejection the click sent {sent}, expected [lua, classic]")
            bar.must(f"click {bar.output} {x + 5:.1f} {y + h / 2:.1f} left")
            fake.wait_for(lambda: len(fake.commands_matching("dispatch")) >= 4)
            sent = fake.commands_matching("dispatch")[3:]
            if sent != [classic]:
                failures.append(f"the next click sent {sent}, expected only the classic form (cached)")

            # The event connection drops: one reconnect, and events still work.
            fake.close_events()
            if not fake.wait_for(lambda: fake.event_connections >= 2):
                failures.append("the bar didn't reconnect to the event socket")
            else:
                del fake.workspaces[base + 1]
                fake.send_event("closewindow>>abc")
                time.sleep(SETTLE_S)
                after = bar.must(f"rects {bar.output}")["rects"].get("workspaces")
                if not after or not after[2] < three[2]:
                    failures.append(f"events after the reconnect didn't update the pills ({three} -> {after})")
    finally:
        fake.close()

    if failures:
        common.fail("; ".join(failures))
    common.ok(f"pills follow events on index {index} (base {base}), unrelated events ignored, "
              "click focuses with the unquoted Lua form, classic fallback cached, event socket reconnects")


if __name__ == "__main__":
    main()
