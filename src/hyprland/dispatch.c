#include "hyprland/dispatch.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hyprland/ipc_request.h"

static bool response_looks_like_error(const char *response) {
    return response && strncmp(response, "error", 5) == 0;
}

// Which dispatch syntax this Hyprland instance actually accepts. Unknown until the
// first call, at which point it's learned once and reused for the rest of the
// process's life -- this Hyprland config's dispatch style doesn't change at
// runtime, so re-probing (and re-logging any rejection) on every single click
// would just be noise.
typedef enum {
    DISPATCH_STYLE_UNKNOWN = 0,
    DISPATCH_STYLE_LUA,
    DISPATCH_STYLE_CLASSIC,
} DispatchStyle;

static DispatchStyle g_dispatch_style = DISPATCH_STYLE_UNKNOWN;

// (hl.dsp.exec_raw("workspace <id>") looks like it should be a config-agnostic
// escape hatch -- it exists specifically to run a classic dispatcher string
// under a Lua config -- but on this Hyprland build (0.56.1) it returns "ok"
// without actually switching the workspace, confirmed by testing it live
// against the socket. Not used for that reason; hl.dsp.focus is what actually
// works, and also matches what the reference Quickshell bar's own click
// handler sends.)

// Sends `lua_cmd`, falling back to `classic_cmd` if this Hyprland rejects the
// Lua syntax -- and remembers which one worked for the rest of the run.
// The Lua form goes first: it's what the reference Quickshell bar sends
// unconditionally, and it's what this machine's Lua-configured Hyprland
// takes, so the common case never eats a failed probe or prints anything.
//
// A failed request (no response at all: the socket is unreachable) says
// nothing about the syntax, so it's not cached either way. The two
// dispatchers used to differ here: the workspace one treated "no response"
// as "the Lua form worked" and cached that, so a single IPC hiccup on the
// first click left a classic-syntax Hyprland on the wrong form for good.
static void dispatch_with_fallback(const char *lua_cmd, const char *classic_cmd, const char *what) {
    if (g_dispatch_style != DISPATCH_STYLE_CLASSIC) {
        char *response = hyprland_ipc_request(lua_cmd);
        if (!response) {
            return; // hyprland_ipc_request() already logged the failure itself
        }
        if (!response_looks_like_error(response)) {
            g_dispatch_style = DISPATCH_STYLE_LUA;
            free(response);
            return;
        }
        if (g_dispatch_style == DISPATCH_STYLE_UNKNOWN) {
            fprintf(stderr, "stickBar: Hyprland rejected the Lua dispatch syntax "
                             "(%s), falling back to the classic form for the rest "
                             "of this run\n", response);
        }
        free(response);
        g_dispatch_style = DISPATCH_STYLE_CLASSIC;
    }

    char *response = hyprland_ipc_request(classic_cmd);
    if (!response) {
        return;
    }
    if (response_looks_like_error(response)) {
        // Both forms failed -- don't fail silently, since this is the only
        // fallback there is.
        fprintf(stderr, "stickBar: Hyprland rejected %s in both syntaxes (%s)\n", what, response);
    }
    free(response);
}

void hyprland_dispatch_focus_workspace(int id) {
    char lua_cmd[96], classic_cmd[96];
    // No quotes around the hl.dsp.focus(...) expression: `hyprctl dispatch '...'`
    // on the CLI only works because the *shell* strips those quotes before
    // hyprctl ever sees them. This code writes straight to the socket with no
    // shell involved, so literal quote characters would reach Hyprland's Lua
    // parser as part of the command text and get rejected ("expected a
    // dispatcher") -- confirmed by testing both forms directly against the socket.
    snprintf(lua_cmd, sizeof(lua_cmd), "dispatch hl.dsp.focus({ workspace = %d })", id);
    snprintf(classic_cmd, sizeof(classic_cmd), "dispatch workspace %d", id);
    char what[48];
    snprintf(what, sizeof(what), "focusing workspace %d", id);
    dispatch_with_fallback(lua_cmd, classic_cmd, what);
}

void hyprland_dispatch_exit(void) {
    // Same Lua-vs-classic ambiguity as the workspace dispatcher -- confirmed
    // live: on this machine's Lua-configured Hyprland, plain "dispatch exit" is
    // parsed as the bareword expression `hl.dispatch(exit)` and rejected
    // ("expected a dispatcher"). The Lua form takes no arguments.
    dispatch_with_fallback("dispatch hl.dsp.exit()", "dispatch exit", "'dispatch exit'");
}
