#include "dbus/sni_watcher_service.h"

#include <stdio.h>
#include <string.h>

#include "dbus/sni_registry.h"

// See sni_watcher.c's own copy of this comment for why the *interface* name is
// always "org.kde.StatusNotifierWatcher" regardless of which well-known *bus
// name* ends up owning the object -- both candidate bus names below expose the
// same interface at the same path.
#define WATCHER_INTERFACE "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"
#define WATCHER_BUS_NAME_KDE "org.kde.StatusNotifierWatcher"
#define WATCHER_BUS_NAME_FDO "org.freedesktop.StatusNotifierWatcher"

static int method_register_item(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)ret_error;
    sd_bus *bus = userdata;

    const char *service = NULL;
    if (sd_bus_message_read(m, "s", &service) < 0 || !service) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "expected a string argument");
    }

    // Two real-world calling conventions, both confirmed against live items
    // during this project's own testing (see sni_registry.c's split_bus_name_path,
    // which parses the equivalent already-combined string on the client side):
    // Steam passes its own object path and expects the *caller's* bus name to be
    // used; EasyEffects (KDE convention) passes a bus name and expects the
    // default "/StatusNotifierItem" path.
    char bus_name[256], path[256];
    if (service[0] == '/') {
        const char *sender = sd_bus_message_get_sender(m);
        snprintf(bus_name, sizeof(bus_name), "%s", sender ? sender : "");
        snprintf(path, sizeof(path), "%s", service);
    } else {
        snprintf(bus_name, sizeof(bus_name), "%s", service);
        snprintf(path, sizeof(path), "/StatusNotifierItem");
    }

    sni_registry_add(bus, bus_name, path);

    char entry[512];
    snprintf(entry, sizeof(entry), "%s%s", bus_name, path);
    sd_bus_emit_signal(bus, WATCHER_PATH, WATCHER_INTERFACE, "StatusNotifierItemRegistered", "s", entry);

    return sd_bus_reply_method_return(m, "");
}

static int method_register_host(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)ret_error;
    sd_bus *bus = userdata;

    // The host's own service-name argument isn't retained -- v1 has no real
    // per-host bookkeeping to do with it (see IsStatusNotifierHostRegistered
    // below, which answers `true` unconditionally rather than tracking real
    // registrations), so the read is only to consume the argument correctly.
    const char *service = NULL;
    sd_bus_message_read(m, "s", &service);

    sd_bus_emit_signal(bus, WATCHER_PATH, WATCHER_INTERFACE, "StatusNotifierHostRegistered", "");
    return sd_bus_reply_method_return(m, "");
}

static int property_get_registered_items(sd_bus *bus, const char *path, const char *interface,
                                          const char *property, sd_bus_message *reply,
                                          void *userdata, sd_bus_error *ret_error) {
    (void)bus; (void)path; (void)interface; (void)property; (void)userdata; (void)ret_error;

    int rc = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "s");
    if (rc < 0) {
        return rc;
    }
    int count = sni_registry_count();
    for (int i = 0; i < count; i++) {
        char entry[512];
        sni_registry_format_entry(sni_registry_get(i), entry, sizeof(entry));
        rc = sd_bus_message_append(reply, "s", entry);
        if (rc < 0) {
            return rc;
        }
    }
    return sd_bus_message_close_container(reply);
}

static int property_get_is_host_registered(sd_bus *bus, const char *path, const char *interface,
                                            const char *property, sd_bus_message *reply,
                                            void *userdata, sd_bus_error *ret_error) {
    (void)bus; (void)path; (void)interface; (void)property; (void)userdata; (void)ret_error;
    // Hardcoded true: this process is always also directly consuming its own
    // registry to render the tray, so this is trivially always true once we're
    // acting as the Watcher -- no need for a real host-registration round-trip
    // through our own connection just to answer this property.
    return sd_bus_message_append(reply, "b", 1);
}

static int property_get_protocol_version(sd_bus *bus, const char *path, const char *interface,
                                          const char *property, sd_bus_message *reply,
                                          void *userdata, sd_bus_error *ret_error) {
    (void)bus; (void)path; (void)interface; (void)property; (void)userdata; (void)ret_error;
    return sd_bus_message_append(reply, "i", 0);
}

static const sd_bus_vtable watcher_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("RegisterStatusNotifierItem", "s", "", method_register_item, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RegisterStatusNotifierHost", "s", "", method_register_host, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as", property_get_registered_items, 0, 0),
    SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b", property_get_is_host_registered, 0,
                     SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ProtocolVersion", "i", property_get_protocol_version, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_SIGNAL("StatusNotifierItemRegistered", "s", 0),
    SD_BUS_SIGNAL("StatusNotifierItemUnregistered", "s", 0),
    SD_BUS_SIGNAL("StatusNotifierHostRegistered", "", 0),
    SD_BUS_VTABLE_END,
};

// Fires for *every* bus-name ownership change, not just ones we care about --
// D-Bus match rules can't filter down to "only names I've registered items
// under", so every call here checks the name against the registry itself.
// Without this, closing an app (e.g. EasyEffects) would leave its icon stuck
// in the tray forever, since nothing else ever tells us its item is gone.
static int on_name_owner_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)userdata; (void)ret_error;
    const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
    if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0) {
        return 0;
    }
    // An empty new_owner means the name's previous owner disconnected (as
    // opposed to the name just changing hands between two owners).
    if (name && new_owner && new_owner[0] == '\0') {
        sni_registry_remove_by_bus_name(name);
    }
    return 0;
}

bool sni_watcher_service_try_start(sd_bus *bus) {
    // sd_bus_request_name() with flags=0 fails (returns -EEXIST, among others)
    // if another connection already owns the name -- exactly the "something
    // else is already the Watcher" case the caller needs to detect and fall
    // back from, so no queueing/replacement flags are passed here.
    if (sd_bus_request_name(bus, WATCHER_BUS_NAME_KDE, 0) < 0) {
        return false;
    }

    sd_bus_slot *vtable_slot = NULL;
    if (sd_bus_add_object_vtable(bus, &vtable_slot, WATCHER_PATH, WATCHER_INTERFACE, watcher_vtable, bus) < 0) {
        fprintf(stderr, "stickBar: claimed %s but failed to register its object -- "
                         "falling back to Host mode\n", WATCHER_BUS_NAME_KDE);
        sd_bus_release_name(bus, WATCHER_BUS_NAME_KDE);
        return false;
    }

    sd_bus_slot *name_owner_slot = NULL;
    sd_bus_match_signal(bus, &name_owner_slot, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                         "org.freedesktop.DBus", "NameOwnerChanged", on_name_owner_changed, NULL);

    // Best-effort second claim: some hosts hardcode the freedesktop.org bus
    // name instead of the (in-practice more common) KDE one. Cheap to also
    // provide since it reuses the same vtable/object registration above;
    // failure is ignored deliberately -- the KDE name is what matters.
    sd_bus_request_name(bus, WATCHER_BUS_NAME_FDO, 0);

    // vtable_slot / name_owner_slot are intentionally left unref'd for the rest
    // of the process's life, same convention as sni_watcher.c's
    // registered_slot/unregistered_slot -- these need to stay alive exactly as
    // long as the bar runs, and the sd_bus connection itself is only torn down
    // once at process exit anyway.
    return true;
}
