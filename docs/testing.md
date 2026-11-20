# Running and testing changes

Build and test commands are in [`CLAUDE.md`](../CLAUDE.md). Two kinds of tests:

- **Unit tests** (`#[cfg(test)]` modules next to the code: parsers, layout maths, date
  maths), run by `cargo test`, which `./build.sh` includes.
- **Live tests** in `tests/`: each starts a second, short-lived bar next to the running one
  and checks it from the outside. What each one checks, and how to add one, is in
  [`tests/README.md`](../tests/README.md).

Beyond the tests, run the bar and watch its stderr for Wayland protocol errors or
Hyprland IPC failures.

## The test-control channel

**Prefer it over ydotool for anything popup-related.** ydotool moved the user's real cursor,
and their clicks kept closing test popups, once while they were in a fullscreen game.

- `STICKBAR_TEST_CONTROL=1` makes stdin a command channel: one command per line in, one JSON
  reply per line out. Without the variable, stdin is never read. The command list is the
  header of `src/testing/mod.rs`; `tests/lib/common.py`'s `ControlledBar` is the Python side.
- Injected input goes through `App::pointer_button`/`pointer_motion`, the same routing real
  pointer events take, so only the compositor's "which surface is under the cursor" step is
  skipped.
- `STICKBAR_TEST_OUTPUT=<name>` keeps the test bar and its popups' catchers on one monitor,
  and in test mode every surface gets an empty input region, so the user's real clicks pass
  straight through.
- stdin is a calloop source. On EOF the source is removed, because a closed pipe stays
  readable and would spin the loop. `tests/test_control_channel.py` checks the channel.

## Running next to another bar

stickBar opens its own Wayland connection and surfaces, and shares no socket, lock file or
singleton with Quickshell or a second stickBar. Every bar is `LAYER_TOP` with
`exclusive_zone=20` anchored top/left/right, and Hyprland adds the zones up: two bars stack
as two 20 px strips (`hyprctl monitors -j`'s `reserved` goes from `[0,20,0,0]` to
`[0,40,0,0]` and back when one exits).

## Cross-reference commands

- **Screenshots:** `grim -g "X,Y WxH" out.png` grabs a region of the whole virtual screen
  (X offsets are cumulative: a 1920-wide first monitor puts the second at x=1920).
  `grim -o <output>` can't be combined with `-g`.
- **Workspaces:** `hyprctl monitors -j`, `hyprctl -j workspaces`, `hyprctl -j activeworkspace`.
- **Volume/mic:** `pactl get-sink-volume @DEFAULT_SINK@`, `get-source-volume
  @DEFAULT_SOURCE@`, `get-{sink,source}-mute`, `list sinks short`, `list sources short`.
  `pactl set-sink-mute @DEFAULT_SINK@ 1` / `set-sink-volume @DEFAULT_SINK@ 30%` trigger a
  live external change, to check the bar follows it without a click.
- **CPU/GPU/memory:** `free -h`, `top`, `nvidia-smi --query-gpu=utilization.gpu,temperature.gpu,memory.used,memory.total,fan.speed,power.draw,pstate --format=csv,noheader`
  (the bar reads the same values in-process through NVML, the library nvidia-smi wraps).
  `awk '/MemTotal/{t=$2} /MemAvailable/{a=$2} END{printf "%.0f\n", 100*(t-a)/t}' /proc/meminfo`
  reproduces the memory box's percent exactly.
- **Media:** `busctl --user list | grep mpris`, `busctl --user introspect <name>
  /org/mpris/MediaPlayer2`, `playerctl -a metadata`; `playerctl play-pause` / `next` /
  `previous` against a real player exercise the PropertiesChanged path.
- **Network:** `ip route show default`, `cat /proc/net/route`, `cat /proc/net/wireless`,
  `ip -4 addr show dev <iface>`, `cat /sys/class/net/<iface>/{operstate,address,mtu,speed,duplex}`
  (this machine: `enp8s0`, Ethernet). With Wi-Fi: `nmcli device wifi`, `iwctl station wlan0 show`.
- **Power menu:** `pkill -SIGUSR1 -x stickBar` is what the Hyprland keybind runs.

## Multi-monitor gotcha

Output indices follow the order the compositor reported the outputs, which isn't
necessarily the left-to-right order `hyprctl monitors -j` lists
([architecture.md](architecture.md)). A click sent to index 0 once landed on the second
physical monitor while the screenshot only covered the first, which looked exactly like "the
click did nothing". Check `outputs` (name, index, width) and `rects <index>` from the
test-control channel, and screenshot that monitor's real x-offset.
