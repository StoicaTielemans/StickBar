#!/usr/bin/env python3
"""The privacy indicator and its dropdown follow a real recording stream.

Records silence from the monitor of a throwaway null sink ("parecord"), so no
real microphone is ever opened (the user's own bar shows the indicator for a
few seconds, though -- it can't tell the difference). Checks:

  - the module's box appears once the stream runs;
  - its dropdown has the maroon accent border, an `in use` pill, and lists the
    stream under MICROPHONE, with one grey-bordered card for the category;
  - a catcher click closes it;
  - when the stream ends, the box collapses again (unless something else on
    this machine is really recording, which isn't touched).
"""

import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402

SINK = "stickbar_privacy_test"
APP = "StickbarTestRecorder"
# Popup-local pixels: OUTER_MARGIN + HEADER_H + SECTION_GAP,
# the first card's top; OUTER_MARGIN, its left border.
CARD_Y, CARD_X = 44, 12
HEADER_MID_Y = 23


def wait_for(pred, timeout=4.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = pred()
        if value:
            return value
        time.sleep(0.1)
    return pred()


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)
    if not shutil.which("parecord") or not shutil.which("pactl"):
        common.skip("needs parecord and pactl")
    failures = []
    load = subprocess.run(["pactl", "load-module", "module-null-sink", f"sink_name={SINK}",
                           f"sink_properties=device.description={SINK}"], capture_output=True, text=True)
    if load.returncode:
        common.skip(f"couldn't load a null sink: {load.stderr.strip()}")
    module = load.stdout.strip()
    recorder = None
    try:
        with common.ControlledBar(binary) as bar, tempfile.TemporaryDirectory() as tmp:
            recorder = subprocess.Popen(["parecord", f"--device={SINK}.monitor", f"--client-name={APP}", "--raw",
                                         "/dev/null"], stderr=subprocess.DEVNULL)
            bar.wait_ready(("privacy",))
            bar.click_module("privacy")
            opened = bar.wait_menu(True)
            entries = opened.get("privacy_entries", [])
            ours = [e for e in entries if e["name"] == APP]
            if not ours or ours[0]["category"] != "MICROPHONE":
                failures.append(f"{APP} not listed under MICROPHONE: {entries}")

            png = str(Path(tmp) / "privacy.png")
            bar.must(f"snapshot {png}")
            width, _height, rows = common.png_pixels(png)
            if not common.close_rgb(rows[0][0], common.COLORS["maroon"]):
                failures.append(f"popup border is {rows[0][0]}")
            if not common.close_rgb(rows[HEADER_MID_Y][width - CARD_X - 3], common.COLORS["maroon"]):
                failures.append(f"pill is {rows[HEADER_MID_Y][width - CARD_X - 3]}")
            if not common.close_rgb(rows[CARD_Y + 8][CARD_X], common.COLORS["surface1"]):
                failures.append(f"card border is {rows[CARD_Y + 8][CARD_X]}")
            if not common.close_rgb(rows[CARD_Y + 4][CARD_X + 3], common.COLORS["base"]):
                failures.append(f"card fill is {rows[CARD_Y + 4][CARD_X + 3]}")

            bar.must("catcher-click")
            bar.wait_menu(False)
            if bar.must("menu").get("catchers"):
                failures.append("catchers left behind")

            recorder.terminate()
            recorder.wait(timeout=5)
            others = [e["name"] for e in entries if e["name"] != APP]
            if others:
                gone = f"box left alone ({others} also recording)"
            else:
                if not wait_for(lambda: "privacy" not in bar.must(f"rects {bar.output}")["rects"]):
                    failures.append("box didn't collapse after the stream ended")
                gone = "box collapsed afterwards"
    finally:
        if recorder and recorder.poll() is None:
            recorder.kill()
        subprocess.run(["pactl", "unload-module", module], capture_output=True)
    if failures:
        common.fail("; ".join(failures))
    common.ok(f"recording stream listed under MICROPHONE in a maroon card dropdown; {gone}")


if __name__ == "__main__":
    main()
