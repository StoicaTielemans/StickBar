#include "dbus/sni_watcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "dbus/sni_registry.h"

// The D-Bus *interface* implemented by every known StatusNotifierWatcher is
// conventionally "org.kde.StatusNotifierWatcher" regardless of which well-known
// *bus name* actually owns it -- a real, if confusing, naming quirk of the SNI
// ecosystem (the interface predates, and was never renamed alongside, the later
// freedesktop.org spec text that documents the same protocol). Both bus-name
// candidates below are therefore called with this same interface name.
#define WATCHER_INTERFACE "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"

// Bus names to try, in order. org.kde.* is confirmed live on this machine
// (owned by the running Quickshell instance, verified via `busctl --user list`).
// org.freedesktop.* is kept as a portability fallback for a system where
// something else provides the Watcher under that name instead -- **unverified**,
// since no such instance exists here to test against. If host registration
// silently fails on a machine that needs this fallback, start here.
static const char *const WATCHER_BUS_NAMES[] = {
    "org.kde.StatusNotifierWatcher",
    "org.freedesktop.StatusNotifierWatcher",
};
#define WATCHER_BUS_NAME_COUNT (sizeof(WATCHER_BUS_NAMES) / sizeof(WATCHER_BUS_NAMES[0]))

// Cached once a working bus name is found, so every later call (the initial item
// fetch, signal subscriptions) reuses it instead of re-probing -- same caching
// convention as hyprland/dispatch.c's DispatchStyle.
static char g_watcher_bus_name[64] = "";

// D-Bus bus names can never contain '/', so the rule is simply: the first '/'
// splits the bus name from the object path; if there's no '/' at all, the whole
// string is the bus name and the path defaults to "/StatusNotifierItem" (the
// conventional default, confirmed against this machine's real registered items).
static void split_bus_name_path(const char *entry, char *bus_name, size_t bus_name_size,
                                 char *path, size_t path_size) {
    const char *slash = strchr(entry, '/');
    if (slash) {
        size_t name_len = (size_t)(slash - entry);
        if (name_len >= bus_name_size) {
            name_len = bus_name_size - 1;
        }
        memcpy(bus_name, entry, name_len);
        bus_name[name_len] = '\0';
        snprintf(path, path_size, "%s", slash);
    } else {
        snprintf(bus_name, bus_name_size, "%s", entry);
        snprintf(path, path_size, "/StatusNotifierItem");
    }
}

static int on_item_registered(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)ret_error;
    sd_bus *bus = userdata;
    const char *entry = NULL;
    if (sd_bus_message_read(m, "s", &entry) < 0 || !entry) {
        return 0;
    }
    char bus_name[256], path[256];
    split_bus_name_path(entry, bus_name, sizeof(bus_name), path, sizeof(path));
    sni_registry_add(bus, bus_name, path);
    return 0;
}

static int on_item_unregistered(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)userdata; (void)ret_error;
    const char *entry = NULL;
    if (sd_bus_message_read(m, "s", &entry) < 0 || !entry) {
        return 0;
    }
    char bus_name[256], path[256];
    split_bus_name_path(entry, bus_name, sizeof(bus_name), path, sizeof(path));
    sni_registry_remove(bus_name, path);
    return 0;
}

static bool try_register_on(sd_bus *bus, const char *watcher_bus_name, const char *host_name) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int rc = sd_bus_call_method(bus, watcher_bus_name, WATCHER_PATH, WATCHER_INTERFACE,
                                 "RegisterStatusNotifierHost", &err, NULL, "s", host_name);
    sd_bus_error_free(&err);
    return rc >= 0;
}

static void fetch_initial_items(sd_bus *bus) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int rc = sd_bus_get_property(bus, g_watcher_bus_name, WATCHER_PATH, WATCHER_INTERFACE,
                                  "RegisteredStatusNotifierItems", &err, &reply, "as");
    if (rc < 0) {
        fprintf(stderr, "gridBar: failed to fetch the initial tray item list (%s)\n",
                err.message ? err.message : "unknown error");
        sd_bus_error_free(&err);
        return;
    }

    char **items = NULL;
    if (sd_bus_message_read_strv(reply, &items) >= 0 && items) {
        for (char **p = items; *p; p++) {
            char bus_name[256], path[256];
            split_bus_name_path(*p, bus_name, sizeof(bus_name), path, sizeof(path));
            sni_registry_add(bus, bus_name, path);
            free(*p);
        }
        free(items);
    }
    sd_bus_message_unref(reply);
}

bool sni_watcher_register_host(sd_bus *bus) {
    char host_name[128];
    snprintf(host_name, sizeof(host_name), "org.freedesktop.StatusNotifierHost-%d-%ld",
             (int)getpid(), (long)time(NULL));
    // Best-effort: the name only needs to be unique enough that RegisterStatusNotifierHost's
    // string argument is meaningful for the Watcher's own bookkeeping/logging --
    // it doesn't need to be a bus name this process is guaranteed to actually own,
    // so a request_name failure here (astronomically unlikely given pid+timestamp
    // anyway) isn't checked.
    sd_bus_request_name(bus, host_name, 0);

    for (size_t i = 0; i < WATCHER_BUS_NAME_COUNT; i++) {
        if (try_register_on(bus, WATCHER_BUS_NAMES[i], host_name)) {
            snprintf(g_watcher_bus_name, sizeof(g_watcher_bus_name), "%s", WATCHER_BUS_NAMES[i]);
            break;
        }
    }

    if (g_watcher_bus_name[0] == '\0') {
        fprintf(stderr, "gridBar: no StatusNotifierWatcher found (tried %s, %s) -- "
                         "system tray will stay empty for this run\n",
                WATCHER_BUS_NAMES[0], WATCHER_BUS_NAMES[1]);
        return false;
    }

    fetch_initial_items(bus);

    sd_bus_slot *registered_slot = NULL, *unregistered_slot = NULL;
    sd_bus_match_signal(bus, &registered_slot, g_watcher_bus_name, WATCHER_PATH, WATCHER_INTERFACE,
                         "StatusNotifierItemRegistered", on_item_registered, bus);
    sd_bus_match_signal(bus, &unregistered_slot, g_watcher_bus_name, WATCHER_PATH, WATCHER_INTERFACE,
                         "StatusNotifierItemUnregistered", on_item_unregistered, bus);
    // Both slots are intentionally left unref'd for the rest of the process's
    // life -- these subscriptions need to stay alive exactly as long as the bar
    // runs, and the sd_bus connection itself is only torn down once at process
    // exit anyway (at which point the whole connection, slots included, goes away).

    return true;
}
