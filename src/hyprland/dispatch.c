#include "hyprland/dispatch.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hyprland/ipc_request.h"

static bool response_looks_like_error(const char *response) {
    return response && strncmp(response, "error", 5) == 0;
}

void hyprland_dispatch_focus_workspace(int id) {
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "dispatch workspace %d", id);

    char *response = hyprland_ipc_request(cmd);
    if (response_looks_like_error(response)) {
        fprintf(stderr, "customBar: 'dispatch workspace %d' rejected (%s), "
                         "retrying with the Lua dispatcher form\n", id, response);
        free(response);

        snprintf(cmd, sizeof(cmd), "dispatch 'hl.dsp.focus({ workspace = %d })'", id);
        response = hyprland_ipc_request(cmd);
    }

    free(response);
}
