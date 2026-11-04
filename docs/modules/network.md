# Network / Wi-Fi

The reference bar's `NetworkWidget.qml` + `NetworkTooltip.qml` wrap a
`NetworkMonitor.qml` singleton that reads `/proc`/`/sys` natively (no persistent
connection to NetworkManager/systemd-networkd/iwd or any other network-management
D-Bus service — the reference doesn't talk to one, and this project doesn't either,
for the same reason: everything it displays is derivable from plain files). This
project's `network/network_client.c` is a direct C port of that same file-reading
approach, structurally the network-domain twin of `sysmon/sysmon_client.c` — same
"re-read everything from scratch on every poll, plain synchronous accessors, no
locking" shape, reusing `main.c`'s existing 5-second sysmon timerfd rather than
adding a second one for the same cadence (`NetworkMonitor.qml`'s own `Timer` uses
the identical 5000ms interval).

- **Default-route discovery**: `/proc/net/route`'s `Destination` column is
  `00000000` for exactly the default route; that row's `Iface` and `Gateway`
  columns give the active interface and its gateway. `Gateway`'s 8-hex-digit
  value packs an IPv4 address as 4 raw bytes in the kernel's own wire order —
  confirmed against this development machine's own real routing table (gateway
  hex `0100A8C0` reverses to `192.168.0.1`, matching `ip route show default`
  exactly), not just reasoned from spec text, using the same reversed-byte-pair
  algorithm the reference `hexToIp()` uses, ported to C in `hex_to_ip()`.
- **Wi-Fi vs. Ethernet detection**: `/proc/net/wireless` lists one line per
  wireless interface (after two header lines) — an interface's mere *presence*
  in that file (matched by its name prefixing a line, after trimming leading
  whitespace) means it's a Wi-Fi device; nothing in this project actually parses
  the quality/signal-level columns that follow, matching the reference's own
  `NetworkMonitor.qml`, which reads the same file for the identical yes/no
  purpose and never surfaces signal strength either.
- **IPv4 address lookup uses `getifaddrs()` (POSIX libc), not a shelled-out
  `ip -4 addr show dev <iface>`** — a deliberate improvement over the reference,
  not just a stylistic port choice. `NetworkMonitor.qml` only re-runs its own `ip
  addr` subprocess when the interface actually changes (its own `ifaceChanged`
  gate, since a subprocess spawn is comparatively expensive and the IP rarely
  changes once assigned); `getifaddrs()` is a single syscall over already
  kernel-resident data, cheap enough to just call on every 5-second poll with no
  equivalent change-gating needed, and it sidesteps interpolating a kernel-
  reported interface name into a shell command string entirely (not a real
  attack surface in practice — interface names are kernel/udev-controlled, not
  user input — but a plain libc call needs no such reasoning about safety at
  all). Same "no subprocess for something the OS already gives you a native call
  for" instinct the reference's own `Time.qml` documents for its native
  `new Date()` vs. shelling out to `date`.
- **rx/tx rate is a delta between the two most recent polls**, sampled against
  `CLOCK_MONOTONIC` (not wall-clock time, unlike the reference's own `Date.now()`
  — immune to an NTP time jump mid-session skewing one sample's elapsed-time
  divisor), reset (no prior sample to diff against) whenever the default-route
  interface itself changes — same self-correcting "0 until the next poll"
  behavior `sysmon_client.c`'s own CPU-jiffies delta already has, and the same
  reason a network-interface change (e.g. Ethernet unplugged, Wi-Fi took over)
  shouldn't diff two byte counters that belong to two different interfaces.
- **Rate history for the dropdown's graph**: `network_client.c` keeps a ring buffer of
  `NETWORK_HISTORY_LEN` rx/tx rates, recorded *always*, not only while the dropdown is
  open, so the graph is full the moment it opens; it costs one store per poll on a
  timer that runs anyway. Cleared when the default-route interface changes or
  disappears (same rule as the rate reset).
- **DNS servers**: every `nameserver` line in `/etc/resolv.conf`.
- **The bar widget (`modules/network/network.c`) shows the connection-state icon
  plus a live rx/tx rate** (`↓12.3 KB/s ↑4.5 KB/s`, or `Disconnected` when the
  interface is down but still has a default route, matching the reference's own
  label text exactly) — the down/up arrows are plain BMP Unicode (`↓`/`↑`, not
  Nerd Font PUA glyphs) but still routed through `glyph_utf8()` with numeric
  codepoints rather than hand-typed as raw literals, per this project's usual
  glyph convention (see "Conventions" in [`CLAUDE.md`](../../CLAUDE.md)), even though these two are common
  enough that hand-typing them would likely have been fine in practice.
- **Accent color is `COLORS.teal`, not the reference's own `Config.colors.green`**
  — a forced deviation, not a preference: this project's own earlier CPU/GPU/
  memory pass already claimed `green` for the GPU module (see [sysmon.md](sysmon.md)), which didn't exist as a conflict in the reference
  (its `SystemMonitorWidget.qml` and `NetworkWidget.qml` are visually
  independent widgets that never needed to coordinate on a shared accent-color
  pool the way this project's per-module color assignment does). `teal` was
  picked from the palette's remaining unclaimed colors as the closest visual
  analogue to green still available.
- **`modules/network/network_info_popup.c`** is the network-domain twin of
  `modules/cpu/cpu_popup.c`, anchored right since the box lives in the bar's right
  region (between the EQ module and Clock -- otherwise matching the reference
  `Bar.qml`'s own `rightLayout` order).
- **Dropdown contents** (it used to be one plain teal text block mirroring
  `NetworkTooltip.qml`), top to bottom:
  - **Header**: status glyph, interface name (bold) + `· Ethernet`/`· Wi-Fi`,
    and a filled pill — `connected` (teal) or `down` (red). With no default
    route, only "No active connection" is shown.
  - **Graph card**: current `↓` rate + `peak` over the history, the download
    half rising to a centre line, the upload half hanging below it, then the
    `↑` rate + peak; an `N min ago … now` axis underneath. Drawn by
    `render/braille_graph.c`: btop's braille look (2×4 dots per cell, one
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
  - **Live while open**: `network_info_popup_refresh_if_open()` runs right
    after `network_client_poll()` in `main.c`'s 5s branch and repaints the
    existing surface, so the graph moves. The size is fixed at open — only
    an interface/type/DNS-count change while open could change the layout,
    which then just clips until reopened.
  - Verified via a scratch build sampling every second and opening the
    popup from a hook, with a `curl` download (curl: ~88 MiB/s; graph peak
    78 MB/s over 1s windows); grid values match `ip`/`/sys`/`resolv.conf`.
- **Not independently verified against a real Wi-Fi adapter** — this development
  machine has only Ethernet (`enp8s0`); every Wi-Fi-specific code path (the
  `/proc/net/wireless` detection, the Wi-Fi icon variants, `LINK SPEED`/duplex
  being omitted) is reasoned from the reference's own logic and
  `/proc/net/wireless`'s documented format, not confirmed live the way the
  Ethernet path's route/gateway/IP parsing was. Worth re-checking first if a bug ever
  shows up specifically on Wi-Fi.
