# Network

Files: `src/network/mod.rs` (`Network`: `poll()` for the box, `refresh_details()` for the
dropdown, and the pure parsers, unit-tested on sample text), `src/modules/network/mod.rs`
(status icon, the box), `src/modules/network/popup.rs` (`NetworkPopup`).

Like the reference's `NetworkMonitor.qml`, everything comes from plain files in `/proc` and
`/sys`: no NetworkManager, systemd-networkd or iwd over D-Bus. Polled on the same 5 s timer as
the system monitor (`App::poll_5s()`, the reference's 5000 ms).

- **Default-route discovery**: `/proc/net/route`'s `Destination` column is
  `00000000` for exactly the default route; that row's `Iface` and `Gateway`
  columns give the active interface and its gateway. `Gateway`'s 8-hex-digit
  value packs an IPv4 address as 4 raw bytes in the kernel's own wire order —
  confirmed against this development machine's own real routing table (gateway
  hex `0100A8C0` reverses to `192.168.0.1`, matching `ip route show default`
  exactly), not just reasoned from spec text, using the same reversed-byte-pair
  algorithm the reference `hexToIp()` uses (`hex_to_ip`).
- **Wi-Fi vs. Ethernet detection**: `/proc/net/wireless` lists one line per
  wireless interface (after two header lines) — an interface's mere *presence*
  in that file (matched by its name prefixing a line, after trimming leading
  whitespace) means it's a Wi-Fi device; nothing in this project actually parses
  the quality/signal-level columns that follow, matching the reference's own
  `NetworkMonitor.qml`, which reads the same file for the identical yes/no
  purpose and never surfaces signal strength either.
- **IPv4 address via `nix::ifaddrs::getifaddrs()`, not a shelled-out
  `ip -4 addr show dev <iface>`** (an improvement over the reference, which runs `ip
  addr` when the interface changes). One libc call over kernel-resident data, safely
  wrapped, and no interface name interpolated into a command line.
- **rx/tx rate is a delta between the two most recent polls**, timed with `Instant`
  (monotonic, unlike the reference's `Date.now()`, so an NTP step can't skew one
  sample). The previous sample is an `Option<(rx, tx, Instant)>`, reset to `None`
  when the default-route interface changes (two interfaces' counters must never be
  diffed), so the rate reads 0 until the next poll, like the CPU's first poll.
- **Rate history for the dropdown's graph**: a `History2` of the last 120 rx/tx rates
  (`util/history.rs`), recorded *always*, not only while the dropdown is
  open, so the graph is full the moment it opens; it costs one store per poll on a
  timer that runs anyway. Cleared when the default-route interface changes or
  disappears (same rule as the rate reset).
- **DNS servers**: every `nameserver` line in `/etc/resolv.conf` (at most 8).
- **The 5s poll reads only what the bar box shows**: the default route and gateway,
  `operstate`, and the rx/tx counters -- both from one read of `/proc/net/dev`
  (the two `/sys/class/net/<iface>/statistics/*_bytes` files it used to read cost
  ~93µs each in kernel time, the whole `/proc/net/dev` about one of them).
  Wi-Fi vs. Ethernet is read only when the interface changes. The dropdown-only
  details (MAC, MTU, speed, duplex, IP, DNS) are read by
  `Network::refresh_details`: when the interface changes, and when the
  dropdown opens or refreshes -- so an IP change from a DHCP renewal shows at the
  dropdown's next open/refresh, not in the bar (which doesn't show it).
- **The box (`modules/network/mod.rs`) shows the connection-state icon plus a live
  rx/tx rate** (`↓12.3 KB/s ↑4.5 KB/s`, or `Disconnected` when the interface is down
  but still has a default route, the reference's label text). The arrows are plain
  Unicode, but still `ARROW_DOWN`/`ARROW_UP` consts in `config/glyphs.rs`, like every
  glyph. With no route the icon is `ICON_NETWORK_UNKNOWN` (0xF06B0, which the installed
  Nerd Font names `md-update`; change it if it looks wrong).
- **Accent is `COLORS.teal`, not the reference's green**: the GPU box already has
  green here ([sysmon.md](sysmon.md)); teal is the closest colour left.
- **The dropdown** (`modules/network/popup.rs`, `NetworkPopup`) is the CPU dropdown's
  twin, anchored right since the box sits in the right region (tray, clock, network,
  volume, mic, updates, from the right edge), top to bottom:
  - **Header**: status glyph, interface name (bold) + `· Ethernet`/`· Wi-Fi`,
    and a filled pill — `connected` (teal) or `down` (red). With no default
    route, only "No active connection" is shown.
  - **Graph card**: current `↓` rate + `peak` over the history, the download
    half rising to a centre line, the upload half hanging below it, then the
    `↑` rate + peak; an `N min ago … now` axis underneath. Drawn by
    `render/braille_graph.rs`: btop's braille look (2×4 dots per cell, one
    sample per dot column, newest at the right),
    dots coloured by height — teal → green → yellow for download, mauve →
    pink → red for upload. Each half scales to its own peak (upload is
    usually far smaller), with a 10 KB/s floor so an idle trickle stays a
    row of single dots instead of filling the graph; any non-zero sample
    lights at least one dot, which gives idle stretches a dotted baseline.
    **The dots are cairo circles, not braille characters**: the Nerd Font
    has all of U+2800..28FF (checked), but drawing them gives crisp, even
    dots and a colour per dot row without one pango layout per cell.
  - **Details grid**: dim `FONT.xs` labels, `text` values — IP, gateway,
    MAC, MTU, link (Ethernet only, the same Wi-Fi omission
    `buildTooltip()` makes), `SINCE BOOT` totals (interface counters, not
    per-session), and DNS servers as small `surface0` tags that wrap.
  - **Live while open**: `refresh_if_open` runs right after the network poll in
    `App::poll_5s()` and repaints the existing surface, so the graph moves. The size is fixed at open — only
    an interface/type/DNS-count change while open could change the layout,
    which then just clips until reopened.
  - Checked with a `curl` download (curl: ~88 MiB/s; graph peak 78 MB/s over
    1 s windows); `test_network_data` checks the figures against
    `ip`/`/sys`/`resolv.conf`.
- **Wi-Fi is not verified against a real adapter**: this machine has only Ethernet
  (`enp8s0`). Every Wi-Fi path (the `/proc/net/wireless` detection, the Wi-Fi icons, link
  speed and duplex left out) follows the reference's logic and the file's documented
  format. Check it first if a bug ever shows up only on Wi-Fi.
