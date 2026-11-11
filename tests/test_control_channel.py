#!/usr/bin/env python3
"""The test-control channel itself works -- every other channel-driven test
builds on it.

Added during the Rust port (M2), where the channel was rebuilt from scratch
before any module existed to exercise it. Checks, on a test-control
instance pinned to one monitor:
  - `ping` replies ok; an unknown command replies ok:false and the bar keeps
    answering afterwards;
  - `outputs` lists exactly the STICKBAR_TEST_OUTPUT monitor, with a bar and
    the width hyprctl reports for it (every other monitor is ignored);
  - `rects` accepts the monitor's name or its index, and rejects a bad name;
  - the bar surface gets an empty input region before its first commit (so
    the user's real clicks pass through a test bar);
  - closing stdin leaves the bar running and idle. A closed pipe stays
    readable forever, so a channel that kept watching it would spin a core.

Passes against both the C bar and the Rust port (`--bin PATH`), which is how
the expected behaviour was confirmed to be the C bar's.
"""

import re
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402

# CPU ticks (1/100 s each) the bar may use in the idle window after stdin
# closes. A spinning loop would use ~200 (a full core for 2 s).
IDLE_WINDOW_S = 2.0
MAX_IDLE_TICKS = 5

# libwayland's debug lines for the requests this test looks for.
_CREATE_REGION = re.compile(r"create_region\(new id wl_region#(\d+)\)")
_REGION_ADD = re.compile(r"wl_region#(\d+)\.add\(")
_SET_INPUT = re.compile(r"wl_surface#(\d+)\.set_input_region\(wl_region#(\d+)\)")
_LAYER = re.compile(r"get_layer_surface\(new id \w+#\d+, wl_surface#(\d+),")
_COMMIT = re.compile(r"wl_surface#(\d+)\.commit\(\)")


def check_input_region(log_path):
    """The layer surface's input region is set to an empty region (one with
    nothing added) before the surface's first commit."""
    empty_regions, isolated, layer_surface = set(), set(), None
    with open(log_path, errors="replace") as f:
        for line in f:
            if m := _CREATE_REGION.search(line):
                empty_regions.add(m.group(1))
            elif m := _REGION_ADD.search(line):
                empty_regions.discard(m.group(1))
            elif m := _SET_INPUT.search(line):
                if m.group(2) in empty_regions:
                    isolated.add(m.group(1))
            elif m := _LAYER.search(line):
                layer_surface = m.group(1)
            elif (m := _COMMIT.search(line)) and m.group(1) == layer_surface:
                if layer_surface not in isolated:
                    common.fail(f"bar surface wl_surface#{layer_surface} committed before "
                                "getting an empty input region")
                return layer_surface
    common.fail("no layer-surface commit found in the WAYLAND_DEBUG log")


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)

    with tempfile.TemporaryDirectory() as tmp:
        log = Path(tmp) / "wayland.log"
        bar = common.ControlledBar(binary, wayland_debug=True, stderr_path=log)
        try:
            if not bar.cmd("ping").get("ok"):
                common.fail("ping didn't reply ok")

            reply = bar.must("outputs")
            outputs = reply["outputs"]
            names = [o["name"] for o in outputs]
            if names != [bar.output]:
                common.fail(f"outputs lists {names}, expected only {bar.output!r}")
            out = outputs[0]
            if not out["has_bar"]:
                common.fail(f"{bar.output} has no bar")
            hypr = {m["name"]: m for m in common.hypr_json("monitors")}
            if bar.output in hypr and out["width"] != hypr[bar.output]["width"]:
                common.fail(f"width {out['width']}, hyprctl says {hypr[bar.output]['width']}")

            bar.must(f"rects {bar.output}")
            bar.must(f"rects {out['index']}")
            if bar.cmd("rects no-such-output").get("ok"):
                common.fail("rects accepted a monitor that doesn't exist")

            bad = bar.cmd("no-such-command")
            if bad.get("ok") or not bad.get("error"):
                common.fail(f"an unknown command got {bad}")
            if not bar.cmd("ping").get("ok"):
                common.fail("the channel stopped answering after an unknown command")

            surface = check_input_region(log)

            # Close stdin: the bar must keep running, without spinning.
            bar.proc.stdin.close()
            time.sleep(0.5)
            before, _ = common.proc_cpu_ticks(bar.proc.pid)
            time.sleep(IDLE_WINDOW_S)
            if bar.proc.poll() is not None:
                common.fail(f"bar exited after stdin closed (status {bar.proc.returncode})")
            after, _ = common.proc_cpu_ticks(bar.proc.pid)
            if after - before > MAX_IDLE_TICKS:
                common.fail(f"bar used {after - before} CPU ticks in {IDLE_WINDOW_S:.0f}s after "
                            "stdin closed -- spinning on EOF")
        finally:
            bar.stop()

    common.ok(f"channel answers on {bar.output} (index {out['index']}, {out['width']}px), "
              f"bar surface wl_surface#{surface} takes no input, idle after stdin closed "
              f"({after - before} ticks in {IDLE_WINDOW_S:.0f}s)")


if __name__ == "__main__":
    main()
