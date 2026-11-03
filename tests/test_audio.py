#!/usr/bin/env python3
"""The volume/mic device dropdowns and the mixer work end to end.

Uses a throwaway null sink (module-null-sink, "StickbarTestSink") and a
silent paplay stream on it, so nothing here changes the volume or mute of a
real device or a real app -- and never the default device (setting a default
would reroute the user's audio, so "click a card to make it default" is the
one path not exercised). Everything is driven through the test-control
channel: no real cursor, no real clicks.

Device dropdown (right-click volume):
  - the test sink appears with its details (state, sample rate, channels);
  - press on its meter at 75% sets its volume to ~75%; dragging to 25% and
    releasing leaves ~25%; scrolling up steps +5%; its mute chip toggles mute.
Mic dropdown: one card per non-monitor source (monitors are hidden), and
the test sink's monitor is flagged as one.
Mixer (middle-click volume):
  - a stream starting while it's open adds a row and the popup grows;
  - a press on that stream's slider at 40% sets the stream to ~40%, its
    mute chip mutes it;
  - the stream ending removes the row and the popup shrinks again.
"""

import json
import os
import subprocess
import sys
import tempfile
import time
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402

SINK = "stickbar_test"
SINK_DESC = "StickbarTestSink"
STREAM = "StickbarTestStream"
TOL = 2  # percent


def pactl(*args):
    return subprocess.run(["pactl", *args], capture_output=True, text=True)


def pactl_json(*args):
    out = pactl("-f", "json", *args).stdout
    return json.loads(out) if out.strip() else []


def percent(volume):
    values = [int(ch["value_percent"].rstrip("%")) for ch in volume.values()]
    return round(sum(values) / len(values))


def sink_state():
    sink = next(s for s in pactl_json("list", "sinks") if s["name"] == SINK)
    return percent(sink["volume"]), sink["mute"]


def stream_state():
    for s in pactl_json("list", "sink-inputs"):
        if s["properties"].get("application.name") == STREAM:
            return percent(s["volume"]), s["mute"]
    return None


def wait_for(pred, timeout=3.0, step=0.1):
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = pred()
        if value:
            return value
        time.sleep(step)
    return pred()


def silent_wav(path, seconds):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(48000)
        w.writeframes(b"\0\0\0\0" * 48000 * seconds)


def press_at(bar, target, release_at=None):
    t = bar.must(target)
    bar.must(f"menu-press {t['x']} {t['y']}")
    if release_at:
        t2 = bar.must(release_at)
        bar.must(f"menu-motion {t2['x']} {t2['y']}")
    bar.must("menu-release")


def check_devices(bar, failures):
    audio = wait_for(lambda: (a := bar.must("dump audio")) and
                     any(s["description"] == SINK_DESC for s in a["sinks"]) and a)
    sink = next((s for s in audio["sinks"] if s["description"] == SINK_DESC), None)
    if not sink:
        failures.append("test sink never appeared in the bar's sink list")
        return
    if sink["channels"] != 2 or sink["rate"] <= 0 or sink["state"] not in ("running", "idle", "suspended"):
        failures.append(f"test sink details wrong: {sink}")
    monitor = next((s for s in audio["sources"] if s["description"] == f"Monitor of {SINK_DESC}"), None)
    if not monitor or not monitor["monitor"]:
        failures.append(f"test sink's monitor not flagged as a monitor: {monitor}")

    bar.click_module("volume", "right")
    menu = bar.wait_menu(True)
    if menu["device_cards"] != len(audio["sinks"]):
        failures.append(f"{menu['device_cards']} device cards for {len(audio['sinks'])} sinks")

    press_at(bar, f"target device meter 0.75 {SINK_DESC}")
    vol = wait_for(lambda: abs(sink_state()[0] - 75) <= TOL and sink_state()[0])
    if not vol:
        failures.append(f"meter press at 75% -> sink volume {sink_state()[0]}%")
    press_at(bar, f"target device meter 0.9 {SINK_DESC}", f"target device meter 0.25 {SINK_DESC}")
    vol = wait_for(lambda: abs(sink_state()[0] - 25) <= TOL and sink_state()[0])
    if not vol:
        failures.append(f"drag 90% -> 25% -> sink volume {sink_state()[0]}%")
    start = sink_state()[0]
    t = bar.must(f"target device body 0 {SINK_DESC}")
    bar.must(f"menu-scroll {t['x']} {t['y']} 1")
    if not wait_for(lambda: sink_state()[0] == start + 5):
        failures.append(f"scroll up on the card: {start}% -> {sink_state()[0]}%")
    t = bar.must(f"target device mute 0 {SINK_DESC}")
    bar.must(f"menu-click {t['x']} {t['y']}")
    if not wait_for(lambda: sink_state()[1]):
        failures.append("mute chip didn't mute the sink")
    bar.must(f"menu-click {t['x']} {t['y']}")
    if not wait_for(lambda: not sink_state()[1]):
        failures.append("mute chip didn't unmute the sink")
    bar.must("catcher-click")
    bar.wait_menu(False)

    bar.click_module("mic", "right")
    menu = bar.wait_menu(True)
    sources = bar.must("dump audio")["sources"]
    expected = sum(1 for s in sources if not s["monitor"] or s["default"])
    if menu["device_cards"] != expected:
        failures.append(f"mic dropdown has {menu['device_cards']} cards, expected {expected} "
                        f"(non-monitor sources) of {len(sources)}")
    bar.must("catcher-click")
    bar.wait_menu(False)


def check_mixer(bar, tmp, failures):
    wav = Path(tmp) / "silence.wav"
    silent_wav(wav, 6)
    bar.click_module("volume", "middle")
    before = bar.wait_menu(True)
    player = subprocess.Popen(["paplay", f"--device={SINK}", f"--property=application.name={STREAM}", str(wav)],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        grown = bar.wait_menu(True, timeout=4.0, mixer_rows=before["mixer_rows"] + 1)
        if grown["height"] <= before["height"] and before["mixer_rows"] > 0:
            failures.append(f"mixer didn't grow for a new stream ({before['height']} -> {grown['height']})")
        if not wait_for(stream_state):
            failures.append("paplay stream never appeared in pactl")
            return
        t = bar.must(f"target mixer slider 0.4 {STREAM}")
        bar.must(f"menu-click {t['x']} {t['y']}")
        if not wait_for(lambda: (s := stream_state()) and abs(s[0] - 40) <= TOL):
            failures.append(f"mixer slider press at 40% -> stream {stream_state()}")
        # Flip and flip back, rather than expecting "muted": PipeWire's
        # stream-restore remembers volume/mute per application.name, so the
        # test stream can start out muted from an earlier run.
        start_muted = stream_state()[1]
        t = bar.must(f"target mixer mute 0 {STREAM}")
        bar.must(f"menu-click {t['x']} {t['y']}")
        if not wait_for(lambda: (s := stream_state()) and s[1] != start_muted):
            failures.append(f"mixer mute chip didn't toggle the stream's mute (was {start_muted})")
        bar.must(f"menu-click {t['x']} {t['y']}")
        if not wait_for(lambda: (s := stream_state()) and s[1] == start_muted):
            failures.append("second click on the mixer mute chip didn't toggle it back")
        player.wait(timeout=10)
        shrunk = bar.wait_menu(True, timeout=4.0, mixer_rows=before["mixer_rows"])
        if shrunk["height"] != before["height"]:
            failures.append(f"mixer didn't shrink back after the stream ended "
                            f"({before['height']} -> {grown['height']} -> {shrunk['height']})")
    finally:
        if player.poll() is None:
            player.kill()
        bar.must("catcher-click")
        bar.wait_menu(False)


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)
    if subprocess.run(["which", "pactl", "paplay"], capture_output=True).returncode != 0:
        common.skip("needs pactl and paplay")
    default_sink = pactl("get-default-sink").stdout.strip()
    default_source = pactl("get-default-source").stdout.strip()
    module = pactl("load-module", "module-null-sink", f"sink_name={SINK}",
                   f"sink_properties=device.description={SINK_DESC}").stdout.strip()
    if not module.isdigit():
        common.skip(f"couldn't load module-null-sink ({module!r})")
    failures = []
    try:
        if pactl("get-default-sink").stdout.strip() != default_sink:
            pactl("set-default-sink", default_sink)
            failures.append("loading the null sink changed the default sink (restored)")
        with tempfile.TemporaryDirectory() as tmp, common.ControlledBar(binary) as bar:
            bar.wait_ready(("volume", "mic"))
            check_devices(bar, failures)
            check_mixer(bar, tmp, failures)
    finally:
        pactl("unload-module", module)
        # Belt and braces: the test must never leave a different default behind.
        if pactl("get-default-sink").stdout.strip() != default_sink:
            pactl("set-default-sink", default_sink)
        if pactl("get-default-source").stdout.strip() != default_source:
            pactl("set-default-source", default_source)
    if failures:
        common.fail("; ".join(failures))
    common.ok("device meter press/drag/scroll/mute on a null sink, mic list hides monitors, "
              "mixer grows/shrinks with a stream and its slider/mute chip work")


if __name__ == "__main__":
    main()
