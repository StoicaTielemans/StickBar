# Bugs

Open bugs first, then the history of fixed ones: what went wrong, why, and how it
was fixed. The rule each one taught is in `docs/`; the story is here. Intended
limitations, as opposed to bugs, are in `docs/limitations.md`.

## Open

- **Power menu (Rust port): `systemctl` starts with SIGINT, SIGTERM and SIGUSR1 blocked.**
  `spawn_systemctl()` in `src/wayland/pointer.rs` uses `std::process::Command`, which doesn't
  reset the signal mask the bar blocks for its signal source (found while porting the EQ, see
  below). Low impact, since `systemctl reboot`/`poweroff` is short-lived, but the same fix as
  the EQ launcher (nix `posix_spawn` with an empty mask) applies.

## Fixed

### EQ

- **`applying` could stay on screen after a failed profile switch** (C bar). If the script
  exited without writing state (e.g. the DAC unplugged), the pending name was only cleared
  when something asked for it after the 15 s `PENDING_TIMEOUT_SEC`, and nothing redrew the
  open dropdown then: the card and yellow pill stayed until the next redraw. Fixed in the
  Rust port by following the script process: its thread reports the exit, which ends
  "applying" at once. Checked with a fake that exits 1 after 2 s without writing state:
  cleared after 2.06 s.
- **Clicking a profile while one was applying started a second script** (C bar). Nothing
  checked for a pending apply, so two `blackpearl-eq.py` processes could run at once and
  steal each other's replies on the DAC's hidraw node. The Rust port ignores profile clicks
  while a script runs or a switch is pending. Checked: a second click during a 2 s fake run
  added nothing to its log.
- **The script started with the bar's blocked signals** (Rust port, before it shipped).
  `docs/porting.md` assumed `std::process::Command` resets the signal mask in the child; it
  doesn't. Found by having a fake script write its `/proc/self/status`: `SigBlk` was
  `0x4202` (SIGINT, SIGUSR1, SIGTERM). Now `posix_spawn` with an empty mask: `SigBlk: 0`.

### Wayland and buffers

- **fd 0 closed by every surface resize.** Every `ShmBuffers` starts out calloc'd/memset
  (so `fd == 0`), and `destroy_buffers_only()` used to `close(sb->fd)` whenever it was
  `>= 0`: the first resize of every surface closed fd 0 -- stdin, then whatever had been
  handed fd 0 next (another surface's memfd, which a later resize then closed again).
  Harmless-looking for months because the bar never read stdin; found when the
  test-control channel's stdin turned out to be `/memfd:stickBar-shm`. The close is now
  tied to `data`. Guard: `tests/test_stdin_intact.py`.
- **Compositor crash turned into a silent 100% CPU spin.** Once the compositor goes
  away, the display fd is permanently readable (EPOLLHUP), so `epoll_wait()` returns
  instantly forever, and `wayland_dispatch_read()` used to ignore
  `wl_display_read_events()` returning -1, with nothing on screen. Verified by running
  a bar through a small Python Unix-socket proxy to the real `wayland-1` (forwarding
  SCM_RIGHTS fds so shm still works) and killing the proxy: the old build kept spinning
  a core, the new one exited within 2s. No nested compositor was installed to test this
  the more obvious way. The proxy socket has to live directly in `$XDG_RUNTIME_DIR` --
  libwayland joins it with `WAYLAND_DISPLAY`, and a longer directory overflows the
  108-byte `sun_path` limit. Guard: `tests/test_wayland_disconnect.py`.
- **Clock seconds lagged real time.** The clock timer used to be a `CLOCK_MONOTONIC`
  interval counted from startup, so the shown seconds lagged by however far into a
  second the bar happened to start (up to ~1s; two bars started at different moments
  visibly disagreed). Measured after the fix with a scratch build logging
  `CLOCK_REALTIME` per tick: every tick lands 0.01-0.13ms after the second boundary.
  Guard: `tests/test_clock_alignment.py`.

### Popups

- **A popup's first paint never happened.** Its first `popup_surface_render_now()` runs
  synchronously from a click handler, long before the compositor's first `configure`
  has been processed back in `main.c`'s event loop. Without the cached draw callback,
  that call silently no-oped and nothing ever repainted it.
- **CPU/GPU/memory dropdowns off-screen.** They were first written copying the tray's
  TOP+RIGHT math, so they grew leftward off the left edge of the screen and were
  invisible (the user caught this: "the popup get cut off the left side of the screen").
  `popup_surface_create()` gained `bottom_margin`/`left_margin` for it, and left-region
  popups now anchor TOP+LEFT.
- **Every open dropdown cost ~19 MB.** Catchers used to get a real full-output
  transparent buffer: ~8.3 MB per 1920x1080 output, so opening any dropdown on this
  2-monitor machine added ~19 MB of RSS (shm) until it closed -- the user noticed the
  bar jump from ~56 MB to ~76 MB. With 1x1 viewporter buffers, measured with the network
  dropdown open: RSS 72 -> 56 MB, shm 16 -> 1 MB. `hyprctl layers -j` still reports the
  catchers as 1920x1040, and a real click on one closed the popup (seen in
  `WAYLAND_DEBUG`: `wl_pointer.button` on the catcher surface, then every popup surface
  destroyed). Guard: `tests/test_popups.py`.
- **A shrinking popup vanished.** A `zwlr_layer_surface_v1_set_size()` + commit was tried
  first (on the mixer) and Hyprland unmapped the popup on shrink (no `closed` event, no
  protocol error, it just vanished) -- confirmed live via `grim` with `paplay` streams
  starting/ending. Destroy-and-recreate was then confirmed for grow → shrink → grow.
  Guards: `tests/test_audio.py`, `tests/test_media.py`.
- **Graphs stopped short of the card's edge.** They used to be a fixed 30 cells (60
  samples, 5 min) centred in the card, and every card is wider than that (text rows set
  the width), so the newest dots stopped well short of the right border -- the user
  pointed it out. Guard: `tests/test_popup_graphs.py`.

### Hyprland IPC

- **Quoted dispatch rejected.** A first version included literal `'...'` quote characters
  in the command text, copied from what works on the CLI. Hyprland's Lua parser silently
  rejected it (`expected a dispatcher`), confirmed by testing both the quoted and
  unquoted forms directly against the socket with a throwaway Python script, bypassing
  `hyprctl` entirely.
- **Rejection logged on every click.** Earlier iterations tried the classic form first
  (which always fails here, needing a fallback + one-time log) and, before the caching
  was added, re-probed and re-logged the rejection on *every single click*.
- **A failed IPC request was cached as "the Lua syntax works".** The workspace and exit
  dispatchers each had their own copy of the probe-and-fall-back logic, and they
  differed: exit returned early when `hyprland_ipc_request()` gave no response, but
  workspace focus passed the NULL to `response_looks_like_error()` (false for NULL)
  and cached `DISPATCH_STYLE_LUA`. So one unreachable-socket moment on the first click
  left a classic-syntax Hyprland on the Lua form for the rest of the run. Found while
  merging the two copies during the code cleanup; both now go through
  `dispatch_with_fallback()`, which caches nothing for a missing response.

### Media

- **Spotify listed twice, sometimes with the wrong song.** Some apps own *two* MPRIS
  names: seen live with Spotify (Electron/Chromium), where PID 57585 owned both
  `org.mpris.MediaPlayer2.spotify` and `org.mpris.MediaPlayer2.chromium.instance57585`
  (checked with `busctl --user call org.freedesktop.DBus ... GetConnectionUnixProcessID`).
  The two reported identical length and position, but the Chromium mirror's *title was
  stale* (an earlier track). Fixed by `is_shadowed()`.
- **Spotify's timeline never appeared.** The spec says `mpris:length` is int64, but
  Spotify sends uint64 (confirmed with `busctl`), so its length stayed 0.
- **Long titles covered neighbouring boxes.** A long title on a narrow output drew over
  (and stole clicks from) its neighbours, until the box was clamped between the side
  regions.

### Power menu

- **Arch logo cut off** (the user reported "the arch logo is cut off").
  `module_box_measure_text()`'s underlying `pango_layout_get_pixel_size()` returns
  Pango's *logical* extent (roughly the cursor-advance width), which for most glyphs
  comfortably contains the visible ink once the usual `+12px` pad is added. A direct
  Pango probe (`pango_layout_get_pixel_extents()`, which returns both ink and logical
  rects) showed this glyph draws ink **14px wide at 14px bold, while its logical/advance
  width is only 8px** -- a 6px gap, far larger than any other glyph here hits (the CPU
  icon is 11px ink vs. 8px logical, comfortably absorbed by the usual padding). Sized
  from the logical width with the reference's tight `+8px` pad, roughly 2px of the
  mantle-colored "cutout" ink fell outside the sapphire box, where it blended invisibly
  into the matching background. Fixed by `module_box_measure_text_ink()` and the standard
  `+12px` pad (the reference's number was tuned against the wrong measurement).
- **Background got darker on hover** (reported by the user). Painted with the default
  `OVER`, every hover-triggered `redraw_all()` blended another 30%-alpha layer on top of
  whatever that shm buffer already held, because each of the two ping-ponged buffers
  still has *its own* previous frame. Every other popup either paints something fully
  opaque or is genuinely transparent (`draw_transparent()`, which already used `SOURCE`), so this was the first *translucent* full-surface
  paint in the codebase. Confirmed with a standalone test: 5 redraws into the same
  un-cleared buffer climbed from alpha 77/255 to 212/255 under `OVER`, while `SOURCE`
  held constant at 77/255 (≈30%).
- **The keybind killed the bar** (the user reported "the power menu crashes the bar").
  SIGUSR1 used to be blocked after `sysmon_client_init()`, which was fine while the GPU
  came from `nvidia-smi`, but NVML's init starts two threads of its own (one named
  `cuda00001c00008`). The kernel delivered the keybind's SIGUSR1 to one of them, and its
  default action killed the bar. Clicking the Arch logo still worked, which is why it
  went unnoticed. Guard: `tests/test_power_menu.py` checks every thread's `SigBlk` and
  sends real SIGUSR1s to a test instance.
- **Panel sat too low.** Centering by `Output.height` ignored that the surface stops at
  the bars' exclusive zones (1040 of 1080 px with two bars running). Unnoticed for a long
  time. Guard: `tests/test_power_menu.py`.

### Tests

- **`test_redraw.py` passed without testing anything.** A first version mapped its time
  window onto `WAYLAND_DEBUG` lines using local time, matched zero commits, and passed
  vacuously. It now fails if its window is empty.
- **`test_power_menu.py` snapshotted a reopened overlay before it had a size.** The
  overlay is compositor-sized, so until its first configure a snapshot fails ("no open
  menu to snapshot"). The first snapshot waited for that; the ones after each
  card-click-and-reopen didn't. It only started failing (~1 run in 4) once the bar's
  text drawing got faster and answered the test channel before the configure arrived.
  Every open now waits (`wait_sized()`).
