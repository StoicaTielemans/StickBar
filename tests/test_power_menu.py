#!/usr/bin/env python3
"""The power menu opens and closes -- and its keybind signal never kills the bar.

The Hyprland keybind sends SIGUSR1 (`pkill -SIGUSR1 -x stickBar`), which the
bar reads through a signalfd. That only works if SIGUSR1 is blocked in
*every* thread: a process-directed signal goes to any thread that doesn't
block it, and SIGUSR1's default action terminates the process. It used to be
blocked only after sysmon_client_init(), whose NVML init starts two threads
of its own -- so the keybind killed the bar (exit by signal 10).

Checks, on a test-control instance (one monitor, never the user's bar --
signals go to its pid only):
  - every thread's /proc status has SIGUSR1 in SigBlk;
  - SIGUSR1 opens the overlay, a second one closes it, and a burst of them
    leaves the bar running;
  - a click on the Arch logo opens it, and a click on the dimmed background
    (well away from the panel) closes it;
  - the panel is centered on its surface, with the accent border on an opaque
    background over the 30% dim, and a hovered card takes its action's colour;
  - a click on the panel's header is absorbed; a click on each card closes the
    menu and picks that card's action.

Those card clicks can't end the session: a test-control instance only records
the action, never runs it (power_menu_handle_click()). As a second layer, that
instance also gets a fake `systemctl` first on its PATH and a Hyprland instance
signature that matches no socket -- and the test fails if the fake ever ran.
The overlay briefly dims the test monitor while it's open.
"""

import os
import tempfile

import signal
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402

SIGUSR1_BIT = 1 << (signal.SIGUSR1 - 1)


def unblocked_threads(pid):
    bad = []
    for task in Path(f"/proc/{pid}/task").iterdir():
        try:
            status = (task / "status").read_text()
        except OSError:
            continue  # thread exited meanwhile
        fields = dict(line.split(":\t", 1) for line in status.splitlines() if ":\t" in line)
        if not int(fields["SigBlk"].strip(), 16) & SIGUSR1_BIT:
            bad.append(f"{task.name} ({fields.get('Name', '?').strip()})")
    return bad


def alive(bar):
    return bar.proc.poll() is None


def check_panel_and_actions(binary, failures):
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        log = tmp / "systemctl.log"
        fake = tmp / "systemctl"
        fake.write_text(f'#!/bin/sh\necho "$@" >> "{log}"\n')
        fake.chmod(0o755)
        env = {"PATH": f"{tmp}:{os.environ.get('PATH', '')}", "HYPRLAND_INSTANCE_SIGNATURE": "stickbar-test-no-such-instance"}
        png = str(tmp / "power.png")
        with common.ControlledBar(binary, env_extra=env) as bar:
            bar.click_module("arch", "left")
            opened = bar.wait_menu(True)
            # The surface is compositor-sized: until its first configure it has
            # no size, and the layout is only a stand-in from the output's size.
            deadline = time.time() + 3.0
            while not opened.get("width") and time.time() < deadline:
                time.sleep(0.05)
                opened = bar.must("menu")

            corner = bar.must("target power corner")
            x, y = int(corner["x"]), int(corner["y"])
            bar.must(f"snapshot {png}")
            width, height, rows = common.png_pixels(png)
            if (width, height) != (opened["width"], opened["height"]):
                failures.append(f"snapshot {width}x{height}, surface {opened['width']}x{opened['height']}")
            right = max(col for col in range(width) if common.close_rgb(rows[y][col], common.COLORS["sapphire"]))
            if abs(x - (width - 1 - right)) > 1:
                failures.append(f"panel not centered horizontally: {x}px left, {width - 1 - right}px right")
            # Equal space above and below: find the bottom border on the panel's left edge.
            bottom = max(row for row in range(height) if common.close_rgb(rows[row][x], common.COLORS["sapphire"]))
            if abs(y - (height - 1 - bottom)) > 1:
                failures.append(f"panel not centered vertically: {y}px above, {height - 1 - bottom}px below")
            if not common.close_rgb(rows[y][x], common.COLORS["sapphire"]) or rows[y][x][3] != 255:
                failures.append(f"panel border is {rows[y][x]}")
            if not common.close_rgb(rows[y + 6][x + 6], common.COLORS["mantle"]) or rows[y + 6][x + 6][3] != 255:
                failures.append(f"panel background is {rows[y + 6][x + 6]}")
            if not 70 <= rows[y][x - 5][3] <= 85:
                failures.append(f"background alpha {rows[y][x - 5][3]}, expected ~77 (30%)")

            header = bar.must("target power header")
            bar.must(f"menu-click {header['x']} {header['y']}")
            bar.wait_menu(True)
            if bar.must("menu")["power_last_action"]:
                failures.append("a header click picked an action")

            for action, color in (("logout", "yellow"), ("restart", "sky"), ("shutdown", "red")):
                t = bar.must(f"target power {action}")
                left, mid = int(t["x"] - 58), int(t["y"])  # CARD_W / 2
                bar.must(f"snapshot {png}")
                _, _, rows = common.png_pixels(png)
                if not common.close_rgb(rows[mid][left], common.COLORS["surface1"]):
                    failures.append(f"{action} card's border is {rows[mid][left]} unhovered")
                bar.must(f"menu-motion {t['x']} {t['y']}")
                bar.must(f"snapshot {png}")
                _, _, rows = common.png_pixels(png)
                if not common.close_rgb(rows[mid][left], common.COLORS[color]):
                    failures.append(f"{action} card's border is {rows[mid][left]} hovered, expected {color}")
                if not common.close_rgb(rows[mid][left + 3], common.COLORS["surface0"]):
                    failures.append(f"{action} card's fill is {rows[mid][left + 3]} hovered")

                bar.must(f"menu-click {t['x']} {t['y']}")
                bar.wait_menu(False)
                picked = bar.must("menu")["power_last_action"]
                if picked != action:
                    failures.append(f"{action} card picked {picked!r}")
                bar.proc.send_signal(signal.SIGUSR1)
                bar.wait_menu(True)
            bar.proc.send_signal(signal.SIGUSR1)
            bar.wait_menu(False)
        if log.exists():
            failures.append(f"a test click REALLY RAN systemctl (the fake): {log.read_text()!r}")


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)
    failures = []
    with common.ControlledBar(binary) as bar:
        bar.wait_ready(("arch",))
        time.sleep(1.0)  # let NVML/PulseAudio/PipeWire start their threads
        threads = len(list(Path(f"/proc/{bar.proc.pid}/task").iterdir()))
        bad = unblocked_threads(bar.proc.pid)
        if bad:
            failures.append(f"SIGUSR1 not blocked in {len(bad)} of {threads} threads: {', '.join(bad)}")

        bar.proc.send_signal(signal.SIGUSR1)
        time.sleep(0.5)
        if not alive(bar):
            common.fail(f"SIGUSR1 killed the bar (exit {bar.proc.returncode}); {'; '.join(failures)}")
        if not bar.must("menu")["open"]:
            failures.append("SIGUSR1 didn't open the power menu")
        bar.proc.send_signal(signal.SIGUSR1)
        time.sleep(0.5)
        if bar.must("menu")["open"]:
            failures.append("a second SIGUSR1 didn't close it")
        for _ in range(10):
            bar.proc.send_signal(signal.SIGUSR1)
            time.sleep(0.05)
        time.sleep(0.5)
        if not alive(bar):
            common.fail(f"a burst of SIGUSR1 killed the bar (exit {bar.proc.returncode})")
        if bar.must("menu")["open"]:  # ten toggles: back to closed
            failures.append("after an even number of toggles the menu is open")

        bar.click_module("arch", "left")
        bar.wait_menu(True)
        bar.must("menu-click 8 8")  # top-left corner: dimmed background, far from the panel
        bar.wait_menu(False)

    check_panel_and_actions(binary, failures)

    if failures:
        common.fail("; ".join(failures))
    common.ok(f"SIGUSR1 blocked in all {threads} threads; keybind signal opens/closes the overlay and a "
              f"burst of 12 leaves the bar running; Arch-logo click opens it, background click closes it; panel centered, hover colours right, "
              f"each card picks its action (recorded, never run)")


if __name__ == "__main__":
    main()
