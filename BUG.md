# Bugs

Open bugs first, then the history of fixed ones: what went wrong, why, and how it
was fixed. The rule each one taught is in `docs/`; the story is here. Intended
limitations, as opposed to bugs, are in `docs/limitations.md`.

## Open

- **EQ: `applying` can stay on screen after a failed profile switch.** If the script
  exits without writing state (e.g. the DAC is unplugged), `eq_state_pending_profile()`
  only clears the pending name when something asks for it after the 15 s
  `PENDING_TIMEOUT_SEC`, and nothing redraws the open dropdown at that moment. The card
  and yellow pill stay until the next redraw, such as moving the pointer over the popup.
  A timer tick while pending, like `media_popup_tick()`, would fix it.
- **EQ: clicking a profile while one is applying starts a second script.**
  `eq_popup_handle_click()` and `eq_state_apply_profile()` don't check for a pending
  apply, so two `blackpearl-eq.py` processes can run at once and steal each other's
  replies on the DAC's hidraw node (see "Why a state file" in `docs/modules/eq.md`). Ignoring
  clicks while something is pending would fix it.
- **Stale comment at `src/eq/eq_state.c:281`.** It says a `SIGCHLD` handler would
  interfere with `sysmon_client.c`'s `popen()`/`pclose()` of `nvidia-smi`, which NVML
  replaced. Harmless, but wrong.

## Fixed

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
