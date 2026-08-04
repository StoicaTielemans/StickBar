#ifndef CUSTOMBAR_HYPRLAND_IPC_REQUEST_H
#define CUSTOMBAR_HYPRLAND_IPC_REQUEST_H

#include <cJSON.h>
#include <stdbool.h>
#include <stddef.h>

// Builds the path to one of Hyprland's per-instance IPC sockets into `out`
// (`filename` is ".socket.sock" for requests, ".socket2.sock" for the event
// stream -- see hyprland/ipc_events.h). Both live under the same
// $XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/ directory, so this is shared
// rather than duplicated between the request and event-stream code. Returns false
// (leaving `out` untouched) if either environment variable is unset, meaning
// Hyprland either isn't running or this process wasn't launched inside its session.
bool hyprland_ipc_socket_path(const char *filename, char *out, size_t out_size);

// Sends `command` verbatim to Hyprland's request socket and returns the raw
// response text as a malloc'd, nul-terminated buffer the caller must free(). One
// request per connection -- Hyprland's IPC doesn't support pipelining multiple
// commands over a single connection; this opens a fresh socket per call. Returns
// NULL on any connection/IO failure (a message is printed to stderr).
char *hyprland_ipc_request(const char *command);

// Convenience wrapper: sends "j/<subcommand>" (Hyprland's JSON-output prefix) and
// parses the response with cJSON. Returns NULL if the request failed or the
// response wasn't valid JSON; the caller must cJSON_Delete() a non-NULL result.
cJSON *hyprland_ipc_request_json(const char *subcommand);

#endif
