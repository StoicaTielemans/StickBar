# GridBar

A from-scratch C status bar for [Hyprland](https://hyprland.org/), built directly
against Wayland (`wl_compositor`/`wl_shm`/`zwlr_layer_shell_v1`) and drawn with
cairo/pangocairo — no GTK, no other UI toolkit.

> [!IMPORTANT]
> **This entire project — every line of C, the build config, and this
> README — was written by an AI coding agent (Claude), not a human.** It
> exists as an experiment in how far an AI agent can carry a real systems
> project (Wayland protocol handling, D-Bus, PulseAudio, MPRIS, raw
> `/proc`/`/sys` parsing) end to end. It has **not** been reviewed line by
> line by an experienced C or Wayland developer.
>
> **Please don't treat this repository as a good reference for how to
> structure a Wayland bar, write idiomatic C, or handle D-Bus/IPC safely.**
> It works (as far as it's been tested), and the code is heavily commented,
> but "an AI wrote it and it compiles and runs" is not the same guarantee as
> "a human who does this professionally reviewed it." If you're looking to
> learn how to build something like this yourself, treat this as one
> possible worked example to read alongside real references (the Wayland
> and wlroots documentation, other hand-written bars like Waybar or the
> Quickshell bar this one is modeled on), not as the source of truth.

## What's in the bar

- **Workspaces** — occupied/focused Hyprland workspace indicator (kanji-numeral
  pills), click to switch.
- **Clock** — current time, right-click for a 12-month year-calendar dropdown.
- **System tray** — StatusNotifierItem icons with DBusMenu right-click menus
  (the usual system tray you'd see in any status bar).
- **Volume / Mic** — default sink/source level, click to mute, scroll to
  adjust, right-click for a device-picker dropdown (PipeWire/PulseAudio).
- **CPU / GPU / Memory** — live usage percentages, right-click each for a
  detailed dropdown (per-core usage, temperatures, VRAM, etc.). GPU stats are
  NVIDIA-only (via `nvidia-smi`) — the module just shows "unavailable" without
  one.
- **Media player** — shows whatever's currently playing over MPRIS (Spotify,
  browsers, VLC, mpv, etc.), click to play/pause, right-click for a dropdown
  listing every open player with per-player controls, a thumbnail, and a
  playback-position timeline.
- **Network / Wi-Fi** — the interface currently holding your default route,
  live upload/download rate, right-click for a dropdown with IP/gateway/MAC/
  DNS details.
- **Power menu** — click the Arch logo (the bar's own leftmost item) for a
  full-screen Logout/Restart/Shutdown overlay, or trigger it from a Hyprland
  keybind (see "Power menu keybind" below).

For a much more detailed writeup of how each module works internally —
including known limitations and deliberate deviations from the reference
Quickshell bar — see [`CLAUDE.md`](./CLAUDE.md).

## Requirements

- **[Hyprland](https://hyprland.org/)**. This isn't just "any wlroots
  compositor" — workspace click-to-focus and live workspace updates go
  through Hyprland's own IPC sockets directly, so the bar's workspace
  indicator won't do anything useful anywhere else (the rest of the bar would
  probably still render on another wlroots compositor with layer-shell
  support, but that's untested).
- **A [Nerd Font](https://www.nerdfonts.com/)** — the bar renders its icons as
  glyphs from a patched font. It's hardcoded to **JetBrainsMono Nerd Font**
  specifically (`src/config/config.c`); install that one, or edit the config
  and rebuild if you want a different Nerd Font.
- A C compiler, [Meson](https://mesonbuild.com/), and
  [Ninja](https://ninja-build.org/) to build it.
- Development libraries: `wayland-client`, `wayland-protocols`, `cairo`,
  `pangocairo`, `cjson`, `libsystemd` (for sd-bus), `libpulse`, and
  `gdk-pixbuf-2.0`.
- Optional, for full functionality: `nvidia-smi` (NVIDIA GPU stats) and a
  running PipeWire/PulseAudio server (volume/mic — most desktops already have
  one via `pipewire-pulse`).

### Installing dependencies (Arch Linux)

```sh
sudo pacman -S --needed base-devel meson ninja wayland wayland-protocols \
  cairo pango cjson systemd-libs libpulse gdk-pixbuf2 ttf-jetbrains-mono-nerd
```

On other distros, install the equivalent `-dev`/`-devel` packages for the
libraries above (e.g. `libwayland-dev`, `libcairo2-dev`, `libpango1.0-dev`,
`libcjson-dev`, `libsystemd-dev`, `libpulse-dev`, `libgdk-pixbuf-2.0-dev` on
Debian/Ubuntu) plus a Nerd Font of your choice from
[nerdfonts.com](https://www.nerdfonts.com/font-downloads).

## Building & running

```sh
meson setup build
meson compile -C build
./build/customBar
```

There's no install step yet — run the built binary directly, or wire it into
your Hyprland config's `exec-once` once you're happy with it:

```
exec-once = /path/to/GridBar/build/customBar
```

It's safe to run alongside another bar (like the Quickshell one it's modeled
on) while you're trying it out — it opens its own independent Wayland
connection and doesn't share any socket or lock file with anything else.

## Power menu keybind

The power menu (Logout/Restart/Shutdown) opens from the Arch logo button on
the bar, or from a Hyprland keybind — the bar listens for `SIGUSR1` and
toggles the menu open/closed when it receives it. Add a line like this to
your `hyprland.conf`:

```
bind = SUPER, Escape, exec, pkill -SIGUSR1 -x customBar
```

Any keybind works — `SUPER, Escape` above is just one reasonable choice.
`pkill -x` matches the process name exactly, so it won't accidentally signal
some unrelated process.

## Known rough edges

This is a first pass, not a finished product — a non-exhaustive list of
what's missing or unpolished:

- No network manager beyond the pure `/proc`/`/sys` reads (no way to change
  Wi-Fi networks, forget a connection, etc. from the bar itself).
- No HiDPI/fractional-scale handling.
- No keyboard handling anywhere, including in the power menu — Escape doesn't
  close it; click the dimmed background instead.
- Several popups don't coordinate with each other (opening two at once, from
  different modules, can leave both open).
- Wi-Fi-specific code paths were written against the spec/reference logic but
  not verified against real Wi-Fi hardware (the development machine only has
  Ethernet).

See `CLAUDE.md` for the full, module-by-module list.
