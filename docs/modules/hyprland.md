# Hyprland IPC and the workspaces box

Files: `src/hyprland/mod.rs` (request socket), `src/hyprland/events.rs` (event socket),
`src/hyprland/dispatch.rs` (`Dispatcher`), `src/modules/workspaces/mod.rs` (the pills). Raw
`AF_UNIX` sockets and serde_json, no IPC library.

- **Request socket** (`$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock`,
  `request` / `request_json`): one connection per command (Hyprland's IPC doesn't pipeline).
  Write the command, `shutdown(Write)` (Hyprland only replies once it sees EOF on the write
  side; skipping this deadlocks), read to EOF. `j/<command>` returns JSON; plain commands like
  `dispatch ...` return text. Requests run on the main loop, so the blocking `UnixStream` has
  a **1 s read/write timeout**: a stuck Hyprland mustn't freeze the bar.
- **Event socket** (`.socket2.sock`): one persistent connection, a calloop source. It
  delivers `event>>payload\n` lines, which may arrive split across reads, so they're
  re-framed. A read containing any event that can change occupancy or focus (`workspace`,
  `workspacev2`, `focusedmon`, `focusedmonv2`, `openwindow`, `closewindow`, `movewindow`,
  `movewindowv2`, `create`/`destroyworkspace[v2]`) triggers one fresh `j/workspaces` +
  `j/activeworkspace` query (no incremental tracking: re-querying is cheap at this event
  rate), and bars redraw only if the occupied set or the focused id changed (most window
  events change neither). On EOF/error the source removes itself and sets
  `App::hyprland_events_lost`, and `main.rs` reconnects exactly once.
- **Occupancy is `windows > 0`** in `j/workspaces`' objects; no need to query clients the way
  the Quickshell version does with `Hyprland.toplevels`.
- **Each monitor shows its own 5 workspaces**: `Output::index` N gets ids `5N+1` to `5N+5`
  (`PILLS_PER_OUTPUT`; the index rules are in [architecture.md](../architecture.md)).
- **Pills are `ClickTarget`s** (`Action::FocusWorkspace`): spots inside the `workspaces` box
  that a left click acts on. The box itself isn't clickable, so the gaps between pills do
  nothing and keep the arrow cursor.

## Dispatch syntax depends on the user's config

`Dispatcher::focus_workspace` (and `Dispatcher::exit`, the power menu's Logout) share one
probe-and-cache path. The rules are in `dispatch.rs`'s module doc, the story in
[`BUG.md`](../../BUG.md) "Hyprland IPC":

- The Lua form `dispatch hl.dsp.focus({ workspace = <id> })` is tried first, **with no
  quotes**: `hyprctl dispatch '...'` works on the command line only because the shell strips
  them; on the raw socket they're literal and Hyprland rejects them. It's what this machine's
  Lua-configured Hyprland (0.56.1) needs, and what the reference bar sends unconditionally.
- If the reply starts with `error`, the classic `dispatch workspace <id>` is sent once, and
  the rejection is logged.
- Whichever form worked is cached for the run. **A missing reply caches nothing.** On this
  machine the first attempt succeeds, so **no stderr output should ever appear in normal
  use**; repeated rejection messages mean this regressed.
- **Don't reach for `hl.dsp.exec_raw("workspace <id>")`.** It looks like the config-agnostic
  escape hatch (the Hyprland wiki suggests it), but on this build it replies `ok` without
  switching. Re-test any such idea directly against the socket first.

## Testing

`tests/test_workspaces.py` runs the bar against `tests/lib/fake_hyprland.py`
(`HYPRLAND_INSTANCE_SIGNATURE` pointing at a temporary socket directory). **Never point a
workspace-click test at the real Hyprland**: it would switch the user's workspace. Clicking
pills on the real bar has been checked by hand.
