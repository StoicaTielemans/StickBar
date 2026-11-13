# StickBar

A from-scratch status bar for [Hyprland](https://hyprland.org/), written in Rust
(no `unsafe`), built directly against Wayland (`wl_compositor`/`wl_shm`/
`zwlr_layer_shell_v1`, through smithay-client-toolkit) and drawn with
cairo/pango — no GTK, no other UI toolkit.

It started as a C bar and was ported to Rust module by module, with the same
look, behaviour and tests (the port's record is [`docs/porting.md`](./docs/porting.md)).
The C version is kept on the `c-legacy` branch.

> [!IMPORTANT]
> **This entire project — every line of C and Rust, the build config, and
> this README — was written by an AI coding agent (Claude), not a human.** It
> exists as an experiment in how far an AI agent can carry a real systems
> project (Wayland protocol handling, D-Bus, PulseAudio, MPRIS, raw
> `/proc`/`/sys` parsing) end to end. It has **not** been reviewed line by
> line by an experienced Rust or Wayland developer.
>
> **Please don't treat this repository as a good reference for how to
> structure a Wayland bar, write idiomatic Rust, or handle D-Bus/IPC safely.**
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
  (the usual system tray you'd see in any status bar), drawn as cards per
  menu section with checkmarks/radio buttons shown.
- **Volume / Mic** — default sink/source level, click to mute, scroll to
  adjust, right-click for a device dropdown (PipeWire/PulseAudio): one card
  per device with its details, a draggable volume meter, a mute toggle, and
  click-to-make-default. Middle-click the volume box for a per-app mixer.
- **CPU / GPU / Memory** — live usage percentages, right-click each for a
  btop-style dropdown: braille history graphs of usage and temperature, plus
  per-thread clocks (P-/E-cores), VRAM/power/fan meters, RAM/swap/zram and
  memory pressure, and the top apps by CPU, VRAM or memory. GPU stats are
  NVIDIA-only (read in-process through NVML, the library behind
  `nvidia-smi`) — the module just shows `--` without one.
- **Media player** — shows whatever's currently playing over MPRIS (Spotify,
  browsers, VLC, mpv, etc.), click to play/pause, right-click for a dropdown
  with a card per open player: title, artist, a live stereo spectrum of that
  player's own audio (braille dots like the network graph, left channel up,
  right channel down), a playback timeline and prev/play-pause/next buttons.
  The spectrum only runs while the dropdown is open — closed, it costs nothing.
- **Network / Wi-Fi** — the interface currently holding your default route,
  live upload/download rate, right-click for a dropdown with a braille
  download/upload graph and IP/gateway/MAC/DNS details.
- **Privacy indicator** — appears only while an app is using the mic, webcam
  or screen sharing (via PipeWire); right-click to see which app.
- **Power menu** — right-click the Arch logo (the bar's own leftmost item) for a
  full-screen Logout/Restart/Shutdown overlay, or trigger it from a Hyprland
  keybind (see "Power menu keybind" below).
- **DAC EQ** — part of the media dropdown: right-click the media box for a TRN
  Black Pearl USB DAC's hardware EQ curve, read from the state file written by
  the companion [DacController](https://github.com/StoicaTielemans/DacController)
  script, and one-click profile chips.

For a much more detailed writeup of how each module works internally —
including known limitations and deliberate deviations from the reference
Quickshell bar — see [`docs/`](./docs), indexed from [`CLAUDE.md`](./CLAUDE.md).

## Requirements

- **[Hyprland](https://hyprland.org/)**. This isn't just "any wlroots
  compositor" — workspace click-to-focus and live workspace updates go
  through Hyprland's own IPC sockets directly, so the bar's workspace
  indicator won't do anything useful anywhere else (the rest of the bar would
  probably still render on another wlroots compositor with layer-shell
  support, but that's untested).
- **A [Nerd Font](https://www.nerdfonts.com/)** — the bar renders its icons as
  glyphs from a patched font. It's hardcoded to **JetBrainsMono Nerd Font**
  specifically (`src/config/fonts.rs`); install that one, or edit the config
  and rebuild if you want a different Nerd Font.
- A Rust toolchain (1.99 or newer, edition 2024; Arch's `rust` package or
  [rustup](https://rustup.rs/)), and `clang` (the PipeWire bindings are
  generated with bindgen at build time). Every other dependency is a crate
  Cargo fetches.
- Libraries the crates link against: `wayland-client`, `cairo`, `pango`,
  `libpulse`, and `libpipewire-0.3` (D-Bus is pure Rust, via zbus).
- Optional, for full functionality: the NVIDIA driver's `libnvidia-ml` (GPU
  stats; loaded at runtime, so the bar starts fine without it) and a
  running PipeWire/PulseAudio server (volume/mic — most desktops already have
  one via `pipewire-pulse`).

### Installing dependencies (Arch Linux)

```sh
sudo pacman -S --needed base-devel rust clang wayland cairo pango \
  libpulse libpipewire ttf-jetbrains-mono-nerd
```

On other distros, install the equivalent `-dev`/`-devel` packages for the
libraries above (e.g. `libwayland-dev`, `libcairo2-dev`, `libpango1.0-dev`,
`libpulse-dev`, `libpipewire-0.3-dev`, `libclang-dev` on Debian/Ubuntu), a Rust
toolchain, plus a Nerd Font of your choice from
[nerdfonts.com](https://www.nerdfonts.com/font-downloads).

## Building & running

```sh
cargo build --release
./target/release/stickBar
```

That's the optimized build to run day to day. For development, `./build.sh`
checks formatting and the (strict) lint policy, runs the unit tests, and builds
both the debug build (`target/debug/stickBar`, what the Python tests in
`tests/` run against) and the release one.

There's no install step yet — run the built binary directly, or wire it into
your Hyprland config's `exec-once` once you're happy with it:

```
exec-once = /path/to/StickBar/target/release/stickBar
```

It's safe to run alongside another bar (like the Quickshell one it's modeled
on) while you're trying it out — it opens its own independent Wayland
connection and doesn't share any socket or lock file with anything else.

## Power menu keybind

The power menu (Logout/Restart/Shutdown) opens from a right click on the Arch
logo on the bar, or from a Hyprland keybind — the bar listens for `SIGUSR1` and
toggles the menu open/closed when it receives it. Add a line like this to
your `hyprland.conf`:

```
bind = SUPER, Escape, exec, pkill -SIGUSR1 -x stickBar
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

See [`docs/limitations.md`](./docs/limitations.md) for the full list.
