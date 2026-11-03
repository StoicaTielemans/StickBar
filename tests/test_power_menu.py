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
    (well away from the three buttons) closes it.
The Logout/Restart/Shutdown buttons themselves are never clicked -- they'd
really run. The overlay briefly dims the test monitor while it's open.
"""

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
        bar.must("menu-click 8 8")  # top-left corner: dimmed background, far from the buttons
        bar.wait_menu(False)

    if failures:
        common.fail("; ".join(failures))
    common.ok(f"SIGUSR1 blocked in all {threads} threads; keybind signal opens/closes the overlay and a "
              f"burst of 12 leaves the bar running; Arch-logo click opens it, background click closes it")


if __name__ == "__main__":
    main()
