# Wayland plumbing gotchas (hit and fixed during initial implementation)

These aren't hypothetical — each actually broke something, and they're worth knowing
before touching this code:

- **Every listener struct field must be non-NULL, including ones for events you
  don't care about.** Binding `wl_output` at version 4 (for `scale`) but only
  filling in `geometry`/`mode`/`done`/`scale` in `wl_output_listener` crashes at
  runtime the moment the compositor sends a `name` or `description` event (added in
  v4) with `listener function for opcode 4 of wl_output is NULL`. `wayland/output.c`
  now has no-op `handle_name`/`handle_description` stubs for exactly this reason —
  if you bump a bind version anywhere, check the interface's full listener struct
  (`awk '/struct X_listener \{/,/^\};/' <header> | grep -E '\(\*\w+\)'`) and fill in
  every field, even as a no-op.
- **`xdg-shell.xml` is linked in even though nothing here uses `xdg_popup`.**
  `zwlr_layer_surface_v1.get_popup(popup: xdg_popup)` makes wayland-scanner's
  generated `wlr-layer-shell-*-protocol.c` reference the `xdg_popup_interface`
  *symbol* (not just the type) as part of that request's `wl_message` signature —
  compiling against a bare forward-declared type works fine, but linking fails with
  `undefined reference to 'xdg_popup_interface'` without xdg-shell's own generated
  code to define it. `meson.build` pulls `xdg-shell.xml` from the system
  `wayland-protocols` package (via `dependency('wayland-protocols')`'s
  `pkgdatadir`) purely to satisfy this, not because anything calls into it — don't
  be surprised its generated header is otherwise unused.
- **`memfd_create()` needs `#define _GNU_SOURCE` before any include, in the file
  that uses it** (`wayland/shm_buffer.c`). `c_std=gnu11` in `meson.build` does *not*
  imply this — that flag only changes which C *language* extensions the compiler
  accepts, not which glibc feature-test macros are predefined. Confirmed by direct
  test: `gcc -std=gnu11` alone gives `implicit declaration of function
  'memfd_create'`.
- **The `wl_display_prepare_read`/`read_events`/`cancel_read` dance
  (`wayland/event_loop.c`) is required, not optional**, because this bar mixes the
  Wayland display fd into an external `epoll_wait()` alongside a timerfd and the
  Hyprland event socket fd. `wl_display_dispatch()` alone always performs a
  blocking read if nothing's queued, which isn't safe to call just because
  `epoll_wait()` said *some* fd was ready.
- **`ShmBuffers.fd` is only valid while `data` is mapped -- never test it
  with `fd >= 0`.** A zeroed `ShmBuffers` has `fd == 0`, which is stdin; the close is
  tied to `data`.
- **A dead Wayland connection must stop the main loop, not be ignored.**
  `wayland_dispatch_prepare()`/`_read()` return `false` on any fatal error
  (`read_events`/`dispatch_pending` returning -1, or a non-`EAGAIN` flush
  failure), log `Wayland connection lost (...)`, and `main.c` breaks out, runs
  the normal teardown, and exits with status 1.

## In the Rust port

- **calloop + `WaylandSource` replace the prepare_read/read_events/cancel_read
  dance.** The source does that dance internally and flushes outgoing requests
  before the loop sleeps. The C rule still holds: a dispatch error means the
  connection is gone, and `main.rs` must end the loop and exit 1, never carry on.
  `test_wayland_disconnect.py` guards it.
- **The system libwayland is the transport** (`wayland-backend`'s `client_system`
  feature). wayland-rs's pure-Rust transport prints `WAYLAND_DEBUG` as
  `[1799736.153][rs] -> wl_surface@11.commit()`. That's a monotonic timestamp and `@`
  instead of `#`, which `tests/lib/common.py` can't parse, and `test_clock_alignment`
  needs the wall-clock time.
- **SCTK's `SlotPool` owns the memfd**, so the C bar's fd-0 bug (closing a zeroed
  `fd`) has no equivalent. `test_stdin_intact.py` still runs as a guard.
- **A busy buffer is detected with `Buffer::canvas()` returning `None`**, the same as
  the C bar's `busy[]` flags. With both buffers held by the compositor, the bar
  stays dirty and draws after the release event.
- **SCTK acks layer-surface configures itself** before calling
  `LayerShellHandler::configure`; don't ack again.
