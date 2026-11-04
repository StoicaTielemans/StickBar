# Hyprland IPC

Two independent connections, both handled without any third-party Wayland-adjacent
IPC library — just raw AF_UNIX sockets + cJSON:

- **Request socket** (`$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock`,
  `hyprland/ipc_request.c`): one connection per command (Hyprland's IPC doesn't
  pipeline). Write the command, `shutdown(fd, SHUT_WR)` (Hyprland only starts
  replying once it sees EOF on the write side — skipping this deadlocks), read to
  EOF, close. `j/<command>` gets JSON back (parsed with cJSON via
  `hyprland_ipc_request_json()`); plain commands like `dispatch ...` get plain text.
- **Event socket** (`.socket2.sock`, `hyprland/ipc_events.c`): one persistent
  non-blocking connection, registered on `main.c`'s epoll instance. Delivers
  `event>>payload\n` lines, which may arrive split across reads — `ipc_events.c`
  buffers and re-frames them. On any event that can change workspace
  occupancy/focus (`workspace`, `workspacev2`, `focusedmon`, `focusedmonv2`,
  `openwindow`, `closewindow`, `movewindow`, `movewindowv2`, `create`/
  `destroyworkspace[v2]`), it calls `workspaces_refresh()` (a fresh
  `j/workspaces` + `j/activeworkspace` query — no incremental delta tracking,
  deliberately, since re-querying is cheap at this event rate) and marks every
  output dirty. If the connection drops (EOF/error), `ipc_events.c` returns `false`
  and **does not retry itself** — `main.c` owns the "attempt exactly one reconnect,
  then give up on live workspace updates for this run" policy, so that policy
  lives in one place instead of being split across files.
- **`hyprctl -j workspaces` objects already carry a `windows` count** — occupancy is
  just `windows > 0`; there's no need to separately query `clients`/toplevels the
  way the Quickshell/QML version does via `Hyprland.toplevels`.
- **Dispatch syntax is compositor-config-dependent, and this matters.**
  `hyprland/dispatch.c`'s `hyprland_dispatch_focus_workspace()` tries
  `dispatch hl.dsp.focus({ workspace = <id> })` **first** — **no surrounding
  quotes**. `hyprctl dispatch '...'` on the CLI only works because the *shell*
  strips those quotes before hyprctl sees them; this code writes straight to the
  raw socket with no shell involved, so literal quotes get rejected. This is confirmed to work on this
  machine's Lua-configured Hyprland (0.56.1), and matches what the reference
  Quickshell bar's own click handler sends **unconditionally, with no fallback at
  all**.
  - **`hl.dsp.exec_raw("workspace <id>")` looks like it should be the "right"
    config-agnostic escape hatch** (it exists specifically to run a classic
    dispatcher string under a Lua config, per the Hyprland wiki and a related
    Waybar issue) — but on this exact build it returns `ok` without actually
    switching the workspace, confirmed live against the socket. **Don't reach for
    it as a fix without re-testing it directly against the socket first** — it
    looked correct on paper and wasn't.
  - Only if the Lua form is rejected does the code fall back once to the classic
    `dispatch workspace <id>` syntax (for compatibility with non-Lua configs), and
    logs to stderr if *that* also fails, instead of failing silently.
  - **Which form actually worked is cached in a static `DispatchStyle` after the
    first call** (a given Hyprland instance's dispatch style doesn't change at
    runtime). On this machine's Lua config, the very first attempt already
    succeeds, so **no stderr output should ever appear in normal use**. If you see
    rejection messages in the log -- especially repeated ones -- this regressed.
