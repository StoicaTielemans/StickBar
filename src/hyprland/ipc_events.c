#include "hyprland/ipc_events.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "hyprland/ipc_request.h" // hyprland_ipc_socket_path()
#include "modules/workspaces/workspaces.h"
#include "wayland/output.h"

// Event lines from Hyprland are short (well under 200 bytes in practice); this cap
// just needs to comfortably hold a burst of several plus one partial trailer.
#define EVENTS_BUF_CAP 8192

struct HyprlandEvents {
    int fd;
    char buf[EVENTS_BUF_CAP];
    size_t buf_len; // bytes of unprocessed data (including any trailing partial line)
};

// Only these event names can change which workspaces are occupied or focused --
// everything else Hyprland emits (activewindow, mouse movement events if any,
// etc.) is irrelevant to this module and ignored.
static bool event_affects_workspaces(const char *event_name) {
    static const char *const relevant[] = {
        "workspace", "workspacev2",
        "focusedmon", "focusedmonv2",
        "openwindow", "closewindow", "movewindow", "movewindowv2",
        "createworkspace", "createworkspacev2",
        "destroyworkspace", "destroyworkspacev2",
        NULL,
    };
    for (int i = 0; relevant[i]; i++) {
        if (strcmp(event_name, relevant[i]) == 0) {
            return true;
        }
    }
    return false;
}

HyprlandEvents *hyprland_events_connect(void) {
    char path[256];
    if (!hyprland_ipc_socket_path(".socket2.sock", path, sizeof(path))) {
        fprintf(stderr, "customBar: cannot locate Hyprland's event socket "
                         "(XDG_RUNTIME_DIR or HYPRLAND_INSTANCE_SIGNATURE unset)\n");
        return NULL;
    }

    // Non-blocking defensively: we only ever read after epoll reports EPOLLIN, so
    // a blocking read shouldn't normally stall, but this avoids any possibility of
    // one doing so on a spurious wakeup or an unlucky race.
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        perror("customBar: socket(hyprland events)");
        return NULL;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("customBar: connect(hyprland events socket)");
        close(fd);
        return NULL;
    }

    HyprlandEvents *ev = calloc(1, sizeof(HyprlandEvents));
    if (!ev) {
        perror("customBar: calloc(HyprlandEvents)");
        close(fd);
        return NULL;
    }
    ev->fd = fd;
    return ev;
}

int hyprland_events_fd(const HyprlandEvents *ev) {
    return ev->fd;
}

void hyprland_events_destroy(HyprlandEvents *ev) {
    if (!ev) {
        return;
    }
    close(ev->fd);
    free(ev);
}

bool hyprland_events_pump(HyprlandEvents *ev, AppContext *app) {
    if (ev->buf_len >= sizeof(ev->buf)) {
        // A pathologically long/unterminated line has filled the entire scratch
        // buffer -- real Hyprland event lines never get anywhere close to this, so
        // treat it as a broken connection rather than looping forever trying to
        // find a newline that isn't coming.
        return false;
    }

    ssize_t n = read(ev->fd, ev->buf + ev->buf_len, sizeof(ev->buf) - ev->buf_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true; // spurious wakeup; nothing to do
        }
        perror("customBar: read(hyprland events)");
        return false;
    }
    if (n == 0) {
        return false; // Hyprland closed the connection (e.g. compositor restarting)
    }
    ev->buf_len += (size_t)n;

    bool workspaces_changed = false;

    // Frame complete "event>>payload\n" lines out of the buffer, leaving any
    // trailing partial line (no '\n' yet) for the next call to complete.
    size_t line_start = 0;
    for (size_t i = 0; i < ev->buf_len; i++) {
        if (ev->buf[i] != '\n') {
            continue;
        }
        ev->buf[i] = '\0';

        const char *line = ev->buf + line_start;
        const char *sep = strstr(line, ">>");
        if (sep) {
            size_t name_len = (size_t)(sep - line);
            char event_name[32];
            if (name_len < sizeof(event_name)) {
                memcpy(event_name, line, name_len);
                event_name[name_len] = '\0';
                if (event_affects_workspaces(event_name)) {
                    workspaces_changed = true;
                }
            }
        }
        line_start = i + 1;
    }

    size_t remaining = ev->buf_len - line_start;
    memmove(ev->buf, ev->buf + line_start, remaining);
    ev->buf_len = remaining;

    if (workspaces_changed) {
        // Re-query rather than trying to apply each event as an incremental delta
        // -- simpler and plenty cheap at the rate these events actually occur.
        workspaces_refresh();
        output_mark_all_dirty(app);
    }

    return true;
}
