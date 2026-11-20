# Architecture

## Layout

One directory per concern (`mod.rs` inside), one directory per bar widget under `modules/`.
Data sources (`sysmon/`, `network/`, `audio/`, `mpris/`, ...) are separate from the widgets
that draw them (`modules/<name>/`).

```
src/
├── main.rs                 entry point: signals first, connect, bind,
│                           the calloop event loop, exit status (0 on SIGINT/SIGTERM,
│                           1 when the compositor connection is lost)
├── wayland/
│   ├── mod.rs              App: all state, the SCTK
│   │                       handler impls (frame callbacks, layer configure/closed,
│   │                       shm, registry), and the delegate macros wiring them up
│   ├── pointer.rs          seat + pointer; App::pointer_button/pointer_motion, the
│   │                       one routing path real and injected input share
│   ├── shm_buffers.rs      ShmBuffers: a surface's two shm buffers (bars + popups)
│   ├── viewporter.rs       optional wp_viewporter bind (SCTK has no helper)
│   └── output.rs           Output: one tracked monitor (index, name, its bar);
│                           OutputHandler: a record + bar per monitor, the
│                           STICKBAR_TEST_OUTPUT filter
├── bar/
│   ├── bar_surface.rs      BarSurface: the layer surface, two shm buffers, the
│   │                       prev/next cairo frames, Dirty (clean/clock/full) +
│   │                       frame-callback pacing, draw -> diff -> commit only the
│   │                       changed rectangle, and the boxes of the last frame
│   ├── damage.rs           changed_rect(): the frame diff, with unit tests
│   ├── popup_surface.rs    PopupSurface: an overlay menu or a top-layer catcher
│   │                       (1x1 + viewporter), first-paint-after-configure
│   ├── popup_host.rs       PopupHost: a menu under its box + a catcher per monitor;
│   │                       resize() recreates the menu at a new size
│   ├── popups.rs           Popups: every dropdown kind; configure/menu routing
│   └── strip_surface.rs    StripSurface: a desync subsurface with its own buffers and
│                           frame pacing (the media graphs)
├── hyprland/
│   ├── mod.rs              socket_path(), request()/request_json(): the request socket
│   ├── events.rs           the event socket as a calloop source; line framing, filter
│   └── dispatch.rs         Dispatcher: Lua form first, classic fallback, cached
├── sysmon/
│   ├── mod.rs              Sysmon: CPU/GPU/memory figures + histories, poll()
│   ├── cpu.rs              /proc/stat deltas, temperature, P/E split, process sample
│   ├── gpu.rs              NVML (nvml-wrapper): usage/temp per poll, details on open
│   ├── memory.rs           /proc/meminfo, PSI, zram, top apps by private memory
│   └── timer.rs            the 5 s poll (calloop Timer), shared with the network
├── audio/
│   ├── mod.rs              AudioState snapshot, Command, Audio (main-side handle)
│   ├── pulse.rs            the pulse thread: libpulse mainloop + cache, channels
│   ├── device_popup.rs     DevicePopup: sinks/sources, meter drag, mute, set default
│   ├── mixer_popup.rs      MixerPopup: per-app streams, slider drag, mute
│   └── volume_style.rs     level icon, meter gradient, slider percent
├── eq/
│   ├── mod.rs              EqState: state.json snapshot (serde_json), pending/running,
│   │                       the inotify watch on the state directory (rustix)
│   ├── launcher.rs         EqState::apply: the eq-script thread, util::spawn,
│   │                       waitpid, exit -> channel
│   └── curve.rs            range_db, freq_to_x, draw: the curve, pure cairo
├── dbus/
│   ├── mod.rs              the `dbus` thread: one async loop (zbus 5) over the merged
│   │                       streams + commands; TrayEvent/Command channels
│   ├── watcher.rs          StatusNotifierWatcher served by hand, or Host mode
│   ├── sni.rs              Registry: items, GetAll (typed visitor), NewIcon, Activate
│   ├── sni_icon.rs         IconPixmap -> premultiplied bytes, symbolic-mask recolour
│   ├── icon_theme.rs       IconName -> PNG bytes (IconThemePath, hicolor, pixmaps)
│   └── dbusmenu.rs         AboutToShow + GetLayout parsing, Event (NoReplyExpected)
├── mpris/mod.rs            Players (on the dbus thread): discovery, PropertiesChanged by
│                           owner, is_shadowed, controls, Position polls on request
├── pipewire/
│   ├── mod.rs              the `pipewire` thread: registry, node + client listeners,
│   │                       PrivacyState only when changed, the visualizer's slots
│   ├── graph.rs            nodes/links/clients, privacy classification, the player ->
│   │                       playback stream match (pure, unit-tested)
│   └── capture.rs          capture streams, the shared rings, /proc ancestry
├── visualizer/
│   ├── mod.rs              Visualizer: slots, smoothing, peaks, synth mode, pump
│   └── spectrum.rs         Hann window, realfft, 82 log bands (pure, unit-tested)
├── network/mod.rs          Network: default route, rates, history; details via
│                           /sys, resolv.conf and getifaddrs (nix)
├── util/history.rs         History2: the 120-sample ring behind every graph
├── util/spawn.rs           starting a child process ("Child processes" below)
├── updates/mod.rs          Updates: checkupdates + yay -Qua on a thread, timers, pacman db watch
├── proxmox/
│   ├── mod.rs              Proxmox: polls the API with curl on a thread, waking, shutdown
│   ├── config.rs           ~/.config/stickBar/proxmox.conf (URL, token, pin, MAC)
│   ├── resources.rs        /cluster/resources JSON -> nodes, guests, storage
│   └── wol.rs              the Wake-on-LAN magic packet
├── shelly/
│   ├── mod.rs              Shelly: plug states, polls while the media dropdown is open
│   ├── config.rs           ~/.config/stickBar/shelly.conf (name = IP per plug)
│   └── http.rs             a tiny HTTP/1.0 client for the plugs' RPC API
├── modules/
│   ├── cpu/                mod.rs (box), popup.rs (the CPU dropdown)
│   ├── gpu/                mod.rs (box), popup.rs (the GPU dropdown)
│   ├── mem/                mod.rs (box), popup.rs (the memory dropdown)
│   ├── network/            mod.rs (box), popup.rs (the network dropdown)
│   ├── media/              mod.rs (Media + the centred box), popup.rs (MediaPopup),
│   │                       graphs.rs (spectrum panels, frames, per-card strips),
│   │                       eq_section.rs (the DAC EQ's dropdown card),
│   │                       shelly_section.rs (the smart-plug card)
│   ├── privacy/            mod.rs (the box), popup.rs (PrivacyPopup)
│   ├── updates/            mod.rs (the box), popup.rs (UpdatesPopup)
│   ├── proxmox/            mod.rs (the box), popup.rs (ProxmoxPopup)
│   ├── tray/               mod.rs (Tray: items, icon surfaces, the icon row),
│   │                       menu.rs (TrayMenu: the DBusMenu dropdown)
│   ├── volume/mod.rs       the volume box
│   ├── mic/mod.rs          the mic box
│   ├── workspaces/mod.rs   Workspaces: occupied/focused from Hyprland, kanji pills
│   ├── powermenu/
│   │   ├── mod.rs          PowerMenu: full-screen overlay per monitor, cards, the
│   │   │                   record-only test guard (actions run in wayland/pointer.rs)
│   │   └── arch_logo.rs    the sapphire Arch block (sized from the glyph's ink)
│   └── clock/
│       ├── mod.rs          Clock: cached text, tick(), its box
│       ├── calendar.rs     CalendarPopup: the year calendar (chrono date maths)
│       └── timer.rs        wall-clock-aligned timerfd (rustix), ECANCELED re-arm
├── render/
│   ├── mod.rs              Renderer (shared by every bar: text cache + modules),
│   │                       render_output(), render_clock_only(), BoxRect/ModuleBox,
│   │                       ClickTarget/Action (clickable spots inside a box)
│   ├── text.rs             TextCache: pango context, font descriptions, LRU of 64
│   │                       shaped layouts; all text goes through here
│   ├── module_box.rs       draw_rect() and label(): the bordered module box
│   ├── braille_graph.rs    btop-style dot graphs (pure cairo)
│   ├── meter_bar.rs        segmented meters
│   ├── stat_card.rs        graph card sizing, axis, label/value and top-apps rows
│   ├── popup_chrome.rs     draw_frame(), draw_card(), draw_header(), draw_pill()
│   └── popup_style.rs      shared dropdown layout tokens (margins, header, pill)
├── testing/mod.rs          the test-control channel on stdin (STICKBAR_TEST_CONTROL=1)
└── config/
    ├── colors.rs           Color + the Catppuccin Mocha palette
    ├── bar.rs              BAR geometry
    ├── fonts.rs            FONT: family and sizes
    └── glyphs.rs           Nerd Font icons as '\u{...}' char consts
```

## How an event becomes pixels

Everything runs on one calloop event loop in `main.rs`: the Wayland connection, timers, the
Hyprland event socket, inotify watches, stdin in test mode, and one channel per worker
thread. SCTK reads the Wayland socket and calls a handler method on `App` (`&mut App`, so
all state is in that one struct). Handlers only mark bars dirty; after each loop pass,
`main.rs` calls `App::flush_dirty()`, which draws each dirty bar that isn't waiting on a
frame callback, so a burst of events in one pass is drawn once.

- **Most sources mark the bar dirty only when what it shows changed**: pulse (default
  volume and mute), PipeWire (the privacy flags), Hyprland (occupied/focused workspaces),
  MPRIS (a player's shown state). The pixel comparison below would drop an unchanged frame
  anyway, but only after laying it out and drawing it. Under event bursts this is what
  keeps the cost down: 300 no-op pulse volume events went from 660 ms to 30 ms of CPU, 20
  PipeWire graph changes from 130 ms to 10 ms.
- **Two private frames per bar (`prev`/`next`) besides the two shm buffers.** `prev` is
  exactly what was last committed; the shm buffers can't serve that role because they
  ping-pong with the compositor, so the one you get back holds a frame from two commits ago.
- **Skip if identical** (`bar/damage.rs`): each redraw draws into `next` and compares it to
  `prev`. No difference means no attach, commit or frame callback at all. This one check
  covers every redraw source (Hyprland events that don't change a pill, 5 s polls landing on
  the same numbers, `pactl` setting a volume to its current value: 40 such calls produce
  zero extra commits, `test_redraw`).
- **Damage is the bounding box of changed pixels**, not the whole surface: a clock tick
  damages about 8x10 px (the last minute digit), a volume change about 32x11.
- **The clock repaints only its own box**, once a minute ([modules/clock.md](modules/clock.md)).
- **Continuous animation goes on a subsurface**, not through this path
  ([popups.md](popups.md)).
- **Text layouts are cached** (`render/text.rs`, an LRU of 64 shaped layouts): every text
  measure and draw used to shape a fresh layout, about 30-45 per output per full redraw.

## Outputs

`Output::index` is handed out in the order the compositor reports outputs, counting every
monitor (it's taken before the `STICKBAR_TEST_OUTPUT` filter, so a test bar's monitor keeps
the index it has in the real bar), and is never reused. It is
not Hyprland's monitor id or the left-to-right order. The workspaces module gives each index
its own range of workspace ids ([modules/hyprland.md](modules/hyprland.md)), and the
test-control channel names monitors by it (`rects 0`).

## Threads

The main thread runs the event loop and draws. Work that blocks or has its own event loop
runs on named threads (`thread::Builder::new().name(..)`) that talk to the main loop through
calloop channels: `pulse`, `pipewire`, `dbus` (tray and MPRIS), `updates-check`,
`proxmox-poll`, `proxmox-power`, `shelly`/`shelly-req`, `eq-script`, `power-action`. Each
starts only after the signal source exists, so it inherits the blocked signals
([modules/power-menu.md](modules/power-menu.md)). NVML starts threads of its own.

## Child processes

The bar blocks SIGINT, SIGTERM and SIGUSR1 in every thread for its signal source, and a
blocked mask survives exec. **Every child goes through `util::spawn`**, never
`std::process::Command`: `Command` keeps the mask, so a child would start unable to be
stopped by SIGTERM or Ctrl-C (it showed as `SigBlk 0x4202` in the child's
`/proc/PID/status`). `util::spawn` uses nix's `posix_spawn` with an empty mask, SIGPIPE back
to its default, and stdin/stderr on `/dev/null` (in test mode the bar's stdin is the
test-control channel, which a child must never read). Programs are named by absolute path,
except `systemctl`, which is looked up in PATH (`spawn_from_path`) so `test_power_menu`'s
fake can take its place. Users: the EQ script, the updates check, Proxmox's curl, the power
menu's `systemctl`. `test_eq`, `test_updates` and `test_proxmox` check that each child's
`SigBlk` is 0.

## Performance

The bar does no GPU work (cairo draws on the CPU into `wl_shm`; Hyprland composites), and
NVML only reads stats: `stickBar` never appears in `nvidia-smi`'s process list. Most of its
memory is NVML ([modules/sysmon.md](modules/sysmon.md)). Measured numbers for every change
are in the progress log of [porting.md](porting.md); how to measure is in
[tests/README.md](../tests/README.md).
