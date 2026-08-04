#ifndef CUSTOMBAR_HYPRLAND_IPC_EVENTS_H
#define CUSTOMBAR_HYPRLAND_IPC_EVENTS_H

#include <stdbool.h>

#include "wayland/context.h"

// Owns a persistent connection to Hyprland's event-stream socket (.socket2.sock)
// plus the small amount of buffering needed to frame "event>>payload\n" lines that
// can arrive split across multiple reads.
typedef struct HyprlandEvents HyprlandEvents;

// Connects (or reconnects) to the event socket. Returns NULL on failure (a message
// is printed to stderr).
HyprlandEvents *hyprland_events_connect(void);

// The underlying fd, for the caller to register on its own epoll instance (EPOLLIN).
int hyprland_events_fd(const HyprlandEvents *ev);

void hyprland_events_destroy(HyprlandEvents *ev);

// Call when epoll reports this connection's fd as readable. Reads whatever's
// available, frames it into complete lines, and for any event that can affect
// workspace occupancy or focus (window open/close/move, workspace/monitor focus
// changes, ...) re-queries Hyprland via workspaces_refresh() and marks every output
// dirty so the next frame reflects the change, rather than trying to apply each
// event as an incremental delta.
//
// Returns false if the connection has dropped (EOF or a real read error) -- the
// caller (main.c) owns what happens next (a single reconnect attempt, then giving
// up on the workspace module -- see the project plan's documented policy). This
// module deliberately doesn't retry on its own, so that policy lives in one place.
bool hyprland_events_pump(HyprlandEvents *ev, AppContext *app);

#endif
