#!/usr/bin/env python3
"""Each media-dropdown card's graph shows its own player's audio, in stereo.

No real audio is involved at all: the test bar runs the visualizer in its
synthetic mode (STICKBAR_TEST_VISUALIZER_SYNTH) -- it never connects to
PipeWire, creates no capture streams, and an active graph slot "hears" sine
tones the test sets over the test-control channel (`vis-tone`) instead.
Everything after the audio is the real code: which slots turn on (the
dropdown open, the card's player playing), the FFT and bands, smoothing,
the dot graph, its own subsurface. Two fake MPRIS players
(tests/lib/fake_mpris.py, silent) provide the cards; they're switched to
Playing over D-Bus, never by a click (which would hit whatever player is
active, possibly the user's own). Player A gets 1kHz in the left channel
only, player B 3kHz in the right channel only. Checks:

  - with the dropdown closed, even while both play and with tones set on
    every slot, every slot's levels stay zero (no visualizer work);
  - opened, A's left half peaks at 1kHz with its right half quiet, B the
    other way round at 3kHz;
  - a PNG of the dropdown shows A's tone column lit near the top of the left
    half, and unlit just under the centre line (its right channel) and at a
    quiet column;
  - when A is paused, its falling graph is committed as frames of the graph's
    own subsurface (the popup itself isn't redrawn), and it settles to zero;
  - closed again: every slot back to zero.

What this can't cover without real audio -- matching a player to its PipeWire
stream, the capture itself, the privacy indicator ignoring it -- is described
in docs/modules/media.md.
"""

import math
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402

FAKE = Path(__file__).resolve().parent / "lib" / "fake_mpris.py"
LIFETIME_S = 40
BANDS = 82  # spectrum.c: log-spaced 50Hz..12kHz, one per dot column
SLOTS = 4   # visualizer.h's VISUALIZER_MAX_SOURCES


def band_of(hz):
    return int(BANDS * math.log(hz / 50) / math.log(12000 / 50))


PLAYERS = {  # name -> (tone Hz, channel index 0=L 1=R)
    "a": (1000, 0),
    "b": (3000, 1),
}
QUIET_BAND = 10  # ~100Hz: far from both tones, and clear of the faded ends
WL_LOG = Path(tempfile.gettempdir()) / f"stickbar_visualizer_wl_{os.getpid()}.log"


def wait_for(pred, timeout=4.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = pred()
        if value:
            return value
        time.sleep(0.1)
    return pred()


def play_pause(name):
    subprocess.run(["gdbus", "call", "--session", "--dest", f"org.mpris.MediaPlayer2.stickbartest_{name}",
                    "--object-path", "/org/mpris/MediaPlayer2", "--method",
                    "org.mpris.MediaPlayer2.Player.PlayPause"], capture_output=True, check=True)


def dot_xy(g, col, row, left):
    """Centre of dot `row` (counted from the centre line outward) in column
    `col` -- render/braille_graph.c's geometry: 8x14px cells, 2x4 dots,
    3px pitch, 1px half-gap at each cell edge."""
    x = g["graph_x"] + (col // 2) * 8 + 1 + (col % 2) * 3 + 1.5
    from_base = (row // 4) * 14 + 1 + (row % 4) * 3 + 1.5
    y = g["top_y"] + g["half_h"] - from_base if left else g["bottom_y"] + from_base
    return int(x), int(y)


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)
    if not shutil.which("gdbus"):
        common.skip("needs gdbus")
    if subprocess.run([sys.executable, "-c", "from gi.repository import Gio"], capture_output=True).returncode:
        common.skip("needs PyGObject (gi) for the fake MPRIS players")

    failures = []
    fakes = []
    frames = 0
    peaks = {}
    try:
        env = {"STICKBAR_TEST_NO_VISUALIZER": "", "STICKBAR_TEST_VISUALIZER_SYNTH": "1",
               "STICKBAR_TEST_VISUALIZER_ONLY": "StickbarTestPlayer-"}
        with tempfile.TemporaryDirectory() as tmp, \
                common.ControlledBar(binary, wayland_debug=True, stderr_path=str(WL_LOG), env_extra=env) as bar:
            if not bar.must("dump visualizer")["available"]:
                common.fail("visualizer unavailable")
            log = Path(tmp) / "fake.log"
            log.write_text("")
            for name in PLAYERS:
                fakes.append(subprocess.Popen([sys.executable, str(FAKE), str(log), str(LIFETIME_S), name]))
            apps = {f"StickbarTestPlayer-{n}" for n in PLAYERS}
            if not wait_for(lambda: apps <= {p["app"] for p in bar.must("dump media")["players"]}):
                common.fail("fake players never showed up")
            for name in PLAYERS:
                play_pause(name)

            # Closed: nothing may run, even with both players playing and a
            # tone set on every slot -- if anything did, levels would move.
            for slot in range(SLOTS):
                bar.must(f"vis-tone {slot} 0 1000")
                bar.must(f"vis-tone {slot} 1 1000")
            time.sleep(1.5)
            if any(any(s["levels_l"]) or any(s["levels_r"]) for s in bar.must("dump visualizer")["slots"]):
                failures.append("levels moving while the dropdown is closed")
            for slot in range(SLOTS):
                bar.must(f"vis-tone {slot} 0 0")
                bar.must(f"vis-tone {slot} 1 0")

            bar.click_module("media")
            bar.wait_menu(True)

            def graphs():
                return {g["app"]: g for g in bar.must("dump visualizer")["graphs"]}

            ours = {n: graphs().get(f"StickbarTestPlayer-{n}") for n in PLAYERS}
            if not all(ours.values()):
                common.skip(f"fake players' cards got no graph (only {SLOTS} slots; cards with graphs: "
                            f"{list(graphs())})")
            for name, (hz, ch) in PLAYERS.items():
                bar.must(f"vis-tone {ours[name]['slot']} {ch} {hz}")

            def separated(name):
                hz, ch = PLAYERS[name]
                g = graphs()[f"StickbarTestPlayer-{name}"]
                b = band_of(hz)
                loud = g["levels_l" if ch == 0 else "levels_r"]
                silent = g["levels_r" if ch == 0 else "levels_l"]
                rest = [v for i, v in enumerate(loud) if abs(i - b) > 2]
                ok = loud[b] > 0.5 and loud[b] > max(rest) + 0.3 and silent[b] < 0.15
                return (loud[b], silent[b]) if ok else None

            for name in PLAYERS:
                peaks[name] = wait_for(lambda: separated(name), timeout=5.0)
                if not peaks[name]:
                    g = graphs()[f"StickbarTestPlayer-{name}"]
                    b = band_of(PLAYERS[name][0])
                    failures.append(f"player {name}: no clean peak at band {b} in its own channel "
                                    f"(L {g['levels_l'][b]:.2f}, R {g['levels_r'][b]:.2f})")

            # Let A's tone column finish rising before taking the picture --
            # the peak check above passes on the way up.
            tone_col = band_of(PLAYERS["a"][0])
            wait_for(lambda: graphs()["StickbarTestPlayer-a"]["levels_l"][tone_col] > 0.95, timeout=3.0)
            g = graphs()["StickbarTestPlayer-a"]
            png = str(Path(tmp) / "media.png")
            bar.must(f"snapshot {png}")
            _w, _h, rows = common.png_pixels(png)
            mantle = common.COLORS["mantle"]
            x, y = dot_xy(g, tone_col, g["rows"] - 3, left=True)
            if common.close_rgb(rows[y][x], mantle):
                failures.append(f"A's tone column has no dot near the top of the left half ({rows[y][x]}, "
                                f"level {g['levels_l'][tone_col]:.2f})")
            x, y = dot_xy(g, tone_col, 0, left=False)
            if not common.close_rgb(rows[y][x], mantle):
                failures.append(f"A's right half lit under the centre line at its tone ({rows[y][x]})")
            x, y = dot_xy(g, QUIET_BAND, g["rows"] - 3, left=True)
            if not common.close_rgb(rows[y][x], mantle):
                failures.append(f"quiet column lit near the top ({rows[y][x]})")

            # A steady tone soon stops changing any dot (unchanged frames
            # aren't committed), so pause A: its slot turns off, its graph
            # falls for ~0.7s, a new frame every tick, on its own subsurface.
            size = f", {int(g['panel_w'])}, {int(g['panel_h'])}"

            def strip_frames():
                return sum(1 for _t, _sid, req, args in common.parse_wayland_log(WL_LOG)
                           if req == "damage_buffer" and args.rstrip(")").endswith(size))

            before = strip_frames()
            play_pause("a")
            time.sleep(1.5)
            frames = strip_frames() - before
            if frames < 5:
                failures.append(f"only {frames} graph subsurface frames while A's graph fell")
            if any(graphs()["StickbarTestPlayer-a"]["levels_l"]):
                failures.append("A's graph didn't settle after pausing")

            bar.must("catcher-click")
            bar.wait_menu(False)
            if not wait_for(lambda: not any(any(s["levels_l"]) or any(s["levels_r"])
                                            for s in bar.must("dump visualizer")["slots"]), timeout=2.0):
                failures.append("levels not reset after closing")
    finally:
        for proc in fakes:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
        WL_LOG.unlink(missing_ok=True)
    if failures:
        common.fail("; ".join(failures))
    pa, pb = peaks["a"], peaks["b"]
    common.ok(f"no real audio used; idle while closed; open: A 1kHz L {pa[0]:.2f}/R {pa[1]:.2f}, B 3kHz R "
              f"{pb[0]:.2f}/L {pb[1]:.2f}, pixels checked, {frames} subsurface frames as A's graph fell; "
              f"idle again after closing")


if __name__ == "__main__":
    main()
