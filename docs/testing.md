# Running and testing changes

Build and test commands are in [`CLAUDE.md`](../CLAUDE.md).

There are no unit tests (almost nothing is headless-testable beyond IPC parsing),
but `tests/` has **live regression tests**: each starts a second, short-lived bar
instance next to the running one, observes it purely from the outside (`/proc`,
`WAYLAND_DEBUG` output, exit status), and stops it — no source patching, so they
test the exact binary that ships. Needs a running Wayland session; stdlib-only
Python. See [`tests/README.md`](../tests/README.md) for what each one checks.

Each test was confirmed to *fail* against the build from before the fix it
guards (commit `1814d4e` for the original five; for the dropdown tests, by
putting each bug back into the current source -- see [`tests/README.md`](../tests/README.md)), not
just to pass now. Beyond that, validate changes
by running the bar and observing it (it's fine to run alongside the existing
Quickshell bar; see below), plus watching stderr for Wayland protocol errors or
Hyprland IPC failures.

- **Test-control channel (`src/testing/test_control.c`) -- prefer it over
  ydotool for anything popup-related.** `STICKBAR_TEST_CONTROL=1` makes stdin
  a command channel (one JSON reply per line on stdout): `rects <output>`
  (module boxes, recorded by `render.c` on each full frame), `click <output>
  <x> <y> <button>`, `menu` (open popup size, catcher count, device/mixer/
  media/EQ card counts, the privacy list, the last power action),
  `menu-press/-click/-motion/-scroll/-release`,
  `catcher-click`, `target device|mixer|media|eq|power ...` (where a named control
  is, from small hooks in those popups), `snapshot <png>` (re-runs the open
  popup's draw callback into an image -- exactly what was painted, no screen
  capture), `refresh <mem|cpu|cpu-reset|gpu|poll>` and `dump
  <mem|cpu|gpu|net|audio|media|eq>`. Injected input goes through
  `pointer_dispatch_button()`/`_motion()`/`_scroll()` -- the same routing
  the wl_pointer listener uses -- so only the compositor's "which surface is
  under the cursor" step is skipped. `STICKBAR_TEST_OUTPUT=<name>` drops
  every other output at its `done` event (no bar, no catchers there), and
  in test mode every surface gets an empty input region, so the user's real
  clicks pass through: **ydotool testing moved the user's real cursor and
  their clicks kept closing test popups -- once while they were in a
  fullscreen game.** All off (and stdin never read) without the env var.
  `tests/lib/common.py`'s `ControlledBar` is the Python side.
- **`WAYLAND_DEBUG` timestamps are UTC wall-clock** (`[HH:MM:SS.micro]`, from
  `CLOCK_REALTIME`), not local time and not monotonic. The sub-second part is
  what makes `test_clock_alignment.py` possible without instrumenting the bar;
  mapping a `time.time()` window onto log lines needs `gmtime()`, not
  `localtime()`.

- **Running alongside Quickshell is safe.** stickBar opens its own independent
  Wayland connection and surfaces; it shares no socket, lock file, or singleton
  resource with Quickshell. Both bars are `LAYER_TOP` with `exclusive_zone=20`
  anchored top/left/right, so wlroots (Hyprland) accumulates the zones additively —
  running both stacks them as two 20px strips (confirmed via
  `hyprctl monitors -j`'s `reserved` field going from `[0,20,0,0]` to `[0,40,0,0]`
  while stickBar runs, and back to `[0,20,0,0]` after it exits cleanly).
- **Screenshotting for visual verification**: `grim` and `slurp` are installed.
  `grim -g "X,Y WxH" out.png` grabs a pixel region across the whole virtual screen
  (multi-monitor X offsets are cumulative, e.g. a 1920-wide first monitor puts the
  second monitor's origin at x=1920) — this is how rendering was checked during
  development, since `grim -o <output>` isn't compatible with `-g`.
- `hyprctl monitors -j` / `hyprctl -j workspaces` / `hyprctl -j activeworkspace` are
  useful for cross-referencing what the bar *should* be showing against what it
  actually renders.
- `pactl get-sink-volume @DEFAULT_SINK@` / `get-source-volume @DEFAULT_SOURCE@` /
  `get-{sink,source}-mute` / `list sinks short` / `list sources short` are the
  equivalent cross-reference for the volume/mic modules — and
  `pactl set-sink-mute @DEFAULT_SINK@ 1` / `set-sink-volume @DEFAULT_SINK@ 30%`
  etc. are a convenient way to trigger a live external change and confirm the
  bar picks it up via its PipeWire subscription without needing to click
  anything (this is how the subscribe → cache → redraw path was verified
  during development).
- `free -h` / `top` / `nvidia-smi` (e.g. `nvidia-smi
  --query-gpu=utilization.gpu,temperature.gpu,memory.used,memory.total,fan.speed,power.draw,pstate
  --format=csv,noheader` -- the bar reads the same values in-process via
  NVML, the library nvidia-smi itself wraps) are the cross-reference for the CPU/GPU/memory
  modules; `awk '/MemTotal/{t=$2} /MemAvailable/{a=$2} END{printf "%.0f\n",
  100*(t-a)/t}' /proc/meminfo` reproduces the memory module's own percent
  calculation exactly, useful for confirming an off-by-something isn't hiding
  in the rounding.
- `busctl --user list | grep mpris` / `busctl --user introspect
  <name> /org/mpris/MediaPlayer2` / `playerctl -a metadata` (if installed)
  are the cross-reference for the media module -- `playerctl play-pause` /
  `next` / `previous` against a real running player (e.g. a browser tab or
  Spotify) exercise its PropertiesChanged subscription the same way.
- `ip route show default` / `cat /proc/net/route` / `cat /proc/net/wireless` /
  `ip -4 addr show dev <iface>` / `cat /sys/class/net/<iface>/{operstate,
  address,mtu,speed,duplex}` are the cross-reference for the network module
  (this machine: `enp8s0`, Ethernet). `nmcli device wifi` / `iwctl station wlan0
  show` are the equivalent on a machine with Wi-Fi.
- `pkill -SIGUSR1 -x stickBar` (with the bar already running) is the same command
  the power menu's Hyprland keybind runs, so it exercises the
  identical code path without adding the keybind first.
- **Multi-monitor debugging gotcha**: this machine's two outputs don't necessarily
  bind in the same left-to-right order `hyprctl monitors -j` lists them in
  (`Output.index` is assigned by *Wayland global binding order*, see
  [architecture.md](architecture.md)). A temporary SIGUSR1-driven debug hook once sent a synthetic click to
  `app.outputs` (the first-bound `Output`), which was the second physical monitor
  (`Output.index == 1`, `HDMI-A-2`), while a `grim -g "0,0 ..."` screenshot only
  captured the first -- it looked identical to "the click did nothing." Log the
  click's `Output.index`/`Output.width` and the recorded hit-rect, and screenshot
  that monitor's actual x-offset, rather than eyeballing coordinates (the memory
  box was once at x≈370-422 on DP-2, not where a downscaled strip screenshot
  suggested).
