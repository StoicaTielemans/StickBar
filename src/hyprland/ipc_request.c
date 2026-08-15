#include "hyprland/ipc_request.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

bool hyprland_ipc_socket_path(const char *filename, char *out, size_t out_size) {
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    const char *instance = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!runtime_dir || !instance) {
        return false;
    }
    int written = snprintf(out, out_size, "%s/hypr/%s/%s", runtime_dir, instance, filename);
    return written > 0 && (size_t)written < out_size;
}

char *hyprland_ipc_request(const char *command) {
    char path[256];
    if (!hyprland_ipc_socket_path(".socket.sock", path, sizeof(path))) {
        fprintf(stderr, "gridBar: XDG_RUNTIME_DIR or HYPRLAND_INSTANCE_SIGNATURE not set "
                         "-- is this running inside a Hyprland session?\n");
        return NULL;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("gridBar: socket(hyprland request)");
        return NULL;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("gridBar: connect(hyprland request socket)");
        close(fd);
        return NULL;
    }

    size_t cmd_len = strlen(command);
    if (write(fd, command, cmd_len) != (ssize_t)cmd_len) {
        perror("gridBar: write(hyprland request)");
        close(fd);
        return NULL;
    }
    // Hyprland only starts replying once it sees EOF on our write side, so
    // shutdown(SHUT_WR) ("I'm done sending") is required here -- without it, this
    // would deadlock waiting to read a response that Hyprland is waiting to send
    // only after seeing us stop writing. The fd stays open for reading afterward.
    shutdown(fd, SHUT_WR);

    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        perror("gridBar: malloc(hyprland response buffer)");
        close(fd);
        return NULL;
    }

    for (;;) {
        if (len + 4096 + 1 > cap) {
            cap *= 2;
            char *grown = realloc(buf, cap);
            if (!grown) {
                perror("gridBar: realloc(hyprland response buffer)");
                free(buf);
                close(fd);
                return NULL;
            }
            buf = grown;
        }
        ssize_t n = read(fd, buf + len, cap - len - 1);
        if (n < 0) {
            perror("gridBar: read(hyprland response)");
            free(buf);
            close(fd);
            return NULL;
        }
        if (n == 0) {
            break; // EOF -- Hyprland closes the connection once its reply is fully sent
        }
        len += (size_t)n;
    }
    buf[len] = '\0';

    close(fd);
    return buf;
}

cJSON *hyprland_ipc_request_json(const char *subcommand) {
    char full_command[128];
    snprintf(full_command, sizeof(full_command), "j/%s", subcommand);

    char *response = hyprland_ipc_request(full_command);
    if (!response) {
        return NULL;
    }

    cJSON *json = cJSON_Parse(response);
    free(response);
    if (!json) {
        fprintf(stderr, "gridBar: hyprctl %s returned invalid JSON\n", full_command);
        return NULL;
    }
    return json;
}
