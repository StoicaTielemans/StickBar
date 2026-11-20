# Wayland plumbing

Rules for `src/wayland/` and the buffer code in `src/bar/`. Each one broke something once
(the stories are in [`BUG.md`](../BUG.md)).

- **calloop's `WaylandSource` drives the connection** (`main.rs`). It reads, dispatches and
  flushes outgoing requests before the loop sleeps, so nothing else may read the display fd.
- **A dead Wayland connection ends the bar.** A dispatch error means the compositor is gone:
  `main.rs` ends the loop and exits with status 1, never carries on.
  `test_wayland_disconnect.py` guards it.
- **The system libwayland is the transport** (`wayland-backend`'s `client_system`
  feature). wayland-rs's pure-Rust transport prints `WAYLAND_DEBUG` as
  `[1799736.153][rs] -> wl_surface@11.commit()`: a monotonic timestamp and `@` instead of
  `#`, which `tests/lib/common.py` can't parse, and `test_clock_alignment` needs the
  wall-clock time.
- **SCTK's `SlotPool` owns the shared-memory file.** Nothing in the bar holds a raw fd for
  it, so the old fd-0 bug (closing a zeroed `fd`, which is stdin) can't come back.
  `test_stdin_intact.py` still runs as a guard.
- **A busy buffer shows as `Buffer::canvas()` returning `None`.** With both buffers held by
  the compositor, the bar stays dirty and draws after the release event.
- **SCTK acks layer-surface configures itself** before calling
  `LayerShellHandler::configure`; don't ack again.
- **A size change arrives as a configure.** The bar surface reallocates its buffers for
  the new size (`bar/bar_surface.rs`); outputs that appear or go away are handled by
  `new_output` / `output_destroyed`.
