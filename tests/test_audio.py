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
  - a stream starting while it's open gets a row, and after it ends the
    row goes -- each time the popup is resized to match (as many cards
    drawn as rows, at the surface's size);
  - a press on that stream's slider at 40% sets the stream to ~40%, its
    mute chip toggles its mute (and back).
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


def card_runs(png, x=15):
    """Cards (runs of `base`) down the popup at column x -- inside the cards'
    left padding, left of any text -- below the header."""
    w, h, px = common.png_pixels(png)
    runs, inside = 0, False
    for y in range(40, h):
        is_card = common.close_rgb(px[y][x], common.COLORS["base"], tol=4)
        runs += 1 if is_card and not inside else 0
        inside = is_card
    return runs, h


def mixer_consistent(bar, tmp, label, failures):
    """The popup was resized to its content: as many cards drawn as rows the
    mixer has, and the snapshot (drawn at the surface's size) matches."""
    menu = bar.must("menu")
    png = Path(tmp) / f"mixer-{label}.png"
    bar.must(f"snapshot {png}")
    cards, h = card_runs(png)
    expected = max(1, menu["mixer_rows"])  # an empty mixer still shows one card
    if cards != expected or h != menu["height"]:
        failures.append(f"mixer {label}: {cards} cards drawn in a {h}px popup for "
                        f"{menu['mixer_rows']} rows (surface {menu['height']}px)")
    return menu


def check_mixer(bar, tmp, failures):
    # Counts of *other* apps' streams can change at any moment (a browser
    # tab starting or stopping sound), so this waits for the test stream by
    # name and checks the popup's size against what it shows, rather than
    # expecting the row count to go up and down by exactly one.
    wav = Path(tmp) / "silence.wav"
    silent_wav(wav, 6)
    bar.click_module("volume", "middle")
    bar.wait_menu(True)
    mixer_consistent(bar, tmp, "before", failures)
    player = subprocess.Popen(["paplay", f"--device={SINK}", f"--property=application.name={STREAM}", str(wav)],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    has_row = lambda: bar.cmd(f"target mixer slider 0.5 {STREAM}").get("ok")  # noqa: E731
    try:
        if not wait_for(has_row, 4.0):
            failures.append("the test stream never got a mixer row")
            return
        time.sleep(0.2)
        with_stream = mixer_consistent(bar, tmp, "with stream", failures)
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
        if not wait_for(lambda: not has_row(), 4.0):
            failures.append("the test stream's row stayed after it ended")
        time.sleep(0.2)
        after = mixer_consistent(bar, tmp, "after", failures)
        if after["mixer_rows"] < with_stream["mixer_rows"] and after["height"] >= with_stream["height"]:
            failures.append(f"mixer didn't shrink: {with_stream['height']} -> {after['height']}px")
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
