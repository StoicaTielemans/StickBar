#!/usr/bin/env python3
"""The bar must exit (status 1) when its compositor connection dies, not spin.

Regression test for wayland/event_loop.c ignoring wl_display_read_events()
failing: a hung-up display fd stays readable forever, so the old code sat at
100% CPU with nothing on screen after a compositor crash/restart.

Runs the bar through tests/lib/wlproxy.py and kills the proxy to simulate the
compositor going away.
"""

import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402

EXIT_DEADLINE_S = 3.0


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)

    runtime = os.environ["XDG_RUNTIME_DIR"]
    real = os.path.join(runtime, os.environ["WAYLAND_DISPLAY"])
    # Must live directly in $XDG_RUNTIME_DIR: libwayland joins it with
    # WAYLAND_DISPLAY, and a long directory overflows sun_path's 108 bytes.
    name = f"stickbar-test-{os.getpid()}.sock"
    sock = os.path.join(runtime, name)

    proxy = subprocess.Popen([sys.executable, "-I", str(Path(__file__).parent / "lib" / "wlproxy.py"), sock, real])
    try:
        for _ in range(50):
            if os.path.exists(sock):
                break
            time.sleep(0.05)
        bar = common.start_bar(binary, env_extra={"WAYLAND_DISPLAY": name})
        try:
            time.sleep(3)
            if bar.poll() is not None:
                common.fail(f"bar exited before the proxy was killed (status {bar.returncode})")

            proxy.kill()
            proxy.wait()
            killed_at = time.monotonic()
            try:
                status = bar.wait(timeout=EXIT_DEADLINE_S)
            except subprocess.TimeoutExpired:
                user, _ = common.proc_cpu_ticks(bar.pid)
                time.sleep(1)
                user2, _ = common.proc_cpu_ticks(bar.pid)
                common.fail(f"bar still running {EXIT_DEADLINE_S}s after the compositor went away "
                            f"(using {user2 - user} CPU ticks/s -- spinning)")
            took = time.monotonic() - killed_at
            if status != 1:
                common.fail(f"bar exited with status {status}, expected 1")
            common.ok(f"bar exited with status 1, {took:.2f}s after the compositor connection died")
        finally:
            common.stop_bar(bar)
    finally:
        if proxy.poll() is None:
            proxy.kill()
        try:
            os.unlink(sock)
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    main()
