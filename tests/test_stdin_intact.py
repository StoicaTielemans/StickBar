#!/usr/bin/env python3
"""The bar must not close file descriptors it never opened.

wayland/shm_buffer.c used to close `sb->fd` whenever it was >= 0 -- but every
ShmBuffers starts out zeroed, so the first resize of every surface closed fd
0 (stdin), and once a memfd had been handed fd 0, later resizes closed *that*
(another surface's pool). Found when the test-control channel's stdin turned
out to be a shm memfd.

Checks from the outside: starts a bar with stdin on a pipe it holds open,
lets it create its surfaces, and checks /proc/<pid>/fd/0 still points at that
pipe (and that no fd points at a memfd more than once per pool).
"""

import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402

SETTLE_S = 3.0


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)

    read_end, write_end = os.pipe()
    pipe_id = os.readlink(f"/proc/self/fd/{read_end}")
    with tempfile.TemporaryDirectory():
        proc = subprocess.Popen([str(binary)], stdin=read_end, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        os.close(read_end)
        try:
            time.sleep(SETTLE_S)
            if proc.poll() is not None:
                common.fail(f"bar exited early (status {proc.returncode})")
            fd0 = os.readlink(f"/proc/{proc.pid}/fd/0")
        finally:
            common.stop_bar(proc)
            os.close(write_end)

    if fd0 != pipe_id:
        common.fail(f"stdin was closed and its fd reused: fd 0 is now {fd0!r} (expected {pipe_id!r})")
    common.ok(f"fd 0 still the original stdin pipe after {SETTLE_S:.0f}s ({fd0})")


if __name__ == "__main__":
    main()
