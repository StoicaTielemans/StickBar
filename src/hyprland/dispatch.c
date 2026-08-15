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

static char *send_lua_form(int id) {
    // No quotes around the hl.dsp.focus(...) expression: `hyprctl dispatch '...'`
    // on the CLI only works because the *shell* strips those quotes before
    // hyprctl ever sees them. This code writes straight to the socket with no
    // shell involved, so literal quote characters would reach Hyprland's Lua
    // parser as part of the command text and get rejected ("expected a
    // dispatcher") -- confirmed by testing both forms directly against the socket.
    //
    // (hl.dsp.exec_raw("workspace <id>") looks like it should be a config-agnostic
    // escape hatch -- it exists specifically to run a classic dispatcher string
    // under a Lua config -- but on this Hyprland build (0.56.1) it returns "ok"
    // without actually switching the workspace, confirmed by testing it live
    // against the socket. Not used here for that reason; hl.dsp.focus is what
    // actually works, and also matches what the reference Quickshell bar's own
    // click handler sends.)
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "dispatch hl.dsp.focus({ workspace = %d })", id);
    return hyprland_ipc_request(cmd);
}

static char *send_classic_form(int id) {
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "dispatch workspace %d", id);
    return hyprland_ipc_request(cmd);
}

void hyprland_dispatch_focus_workspace(int id) {
    // Try the Lua form first -- it's what the reference Quickshell bar sends
    // unconditionally (no fallback at all), and it's confirmed to work on this
    // machine's Lua-configured Hyprland. This ordering means the common case (a
    // Lua config, like this machine's) never has to eat a failed probe attempt or
    // print anything to stderr.
    if (g_dispatch_style != DISPATCH_STYLE_CLASSIC) {
        char *response = send_lua_form(id);
        if (!response_looks_like_error(response)) {
            g_dispatch_style = DISPATCH_STYLE_LUA;
            free(response);
            return;
        }

        if (g_dispatch_style == DISPATCH_STYLE_UNKNOWN) {
            fprintf(stderr, "gridBar: Hyprland rejected the Lua dispatch syntax "
                             "(%s), falling back to the classic form for the rest "
                             "of this run\n", response);
        }
        free(response);
        g_dispatch_style = DISPATCH_STYLE_CLASSIC;
    }

    char *response = send_classic_form(id);
    if (response_looks_like_error(response)) {
        // Both forms failed -- don't fail silently, since this is the only
        // fallback we have.
        fprintf(stderr, "gridBar: classic dispatch syntax also rejected (%s); "
                         "workspace %d was not focused\n", response, id);
    }
    free(response);
}

void hyprland_dispatch_exit(void) {
    // Same Lua-vs-classic ambiguity as hyprland_dispatch_focus_workspace() above --
    // confirmed live, not just theorized: on this machine's Lua-configured Hyprland,
    // plain "dispatch exit" is parsed as the bareword expression `hl.dispatch(exit)`
    // and rejected ("expected a dispatcher"), the same failure mode the workspace
    // dispatcher already works around. The Lua form here is "dispatch hl.dsp.exit()"
    // (no arguments, unlike hl.dsp.focus's workspace table). Reuses the same cached
    // g_dispatch_style as the workspace dispatcher, since it's the same Hyprland
    // instance and its dispatch style doesn't change at runtime.
    if (g_dispatch_style != DISPATCH_STYLE_CLASSIC) {
        char *response = hyprland_ipc_request("dispatch hl.dsp.exit()");
        if (!response) {
            return; // hyprland_ipc_request() already logged the failure itself
        }
        if (!response_looks_like_error(response)) {
            g_dispatch_style = DISPATCH_STYLE_LUA;
            free(response);
            return;
        }

        if (g_dispatch_style == DISPATCH_STYLE_UNKNOWN) {
            fprintf(stderr, "gridBar: Hyprland rejected the Lua dispatch syntax "
                             "(%s), falling back to the classic form for the rest "
                             "of this run\n", response);
        }
        free(response);
        g_dispatch_style = DISPATCH_STYLE_CLASSIC;
    }

    char *response = hyprland_ipc_request("dispatch exit");
    if (!response) {
        return; // hyprland_ipc_request() already logged the failure itself
    }
    if (response_looks_like_error(response)) {
        fprintf(stderr, "gridBar: Hyprland rejected 'dispatch exit' (%s)\n", response);
    }
    free(response);
}
