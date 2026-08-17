#include "dbus/dbusmenu.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MENU_INTERFACE "com.canonical.dbusmenu"

// Parses one child entry positioned with the read cursor already inside its
// "ia{sv}av" struct (id not yet read). Leaves the cursor past the struct's
// content on return (caller still owns exiting the struct/variant containers).
static void parse_entry(sd_bus_message *m, DbusMenuEntry *entry) {
    memset(entry, 0, sizeof(*entry));
    entry->enabled = true; // DBusMenu convention: absent "enabled" means enabled

    sd_bus_message_read_basic(m, SD_BUS_TYPE_INT32, &entry->id);

    if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}") > 0) {
        while (sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv") > 0) {
            const char *key = NULL;
            sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &key);

            char type = 0;
            const char *contents = NULL;
            sd_bus_message_peek_type(m, &type, &contents);

            if (contents && sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, contents) > 0) {
                if (key && strcmp(key, "type") == 0 && strcmp(contents, "s") == 0) {
                    const char *type_str = NULL;
                    sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &type_str);
                    if (type_str && strcmp(type_str, "separator") == 0) {
                        entry->is_separator = true;
                    }
                } else if (key && strcmp(key, "label") == 0 && strcmp(contents, "s") == 0) {
                    const char *label = NULL;
                    sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &label);
                    if (label) {
                        snprintf(entry->label, sizeof(entry->label), "%s", label);
                    }
                } else if (key && strcmp(key, "enabled") == 0 && strcmp(contents, "b") == 0) {
                    int enabled = 1;
                    sd_bus_message_read_basic(m, SD_BUS_TYPE_BOOLEAN, &enabled);
                    entry->enabled = enabled != 0;
                } else {
                    // Every other property (icon-name, toggle-type, toggle-state,
                    // children-display, ...) is intentionally unhandled -- this
                    // matches the reference TrayContextMenu.qml, which doesn't
                    // render icons or checkmarks for menu entries either, despite
                    // real live data (confirmed via busctl) including a
                    // toggle-type/toggle-state entry.
                    sd_bus_message_skip(m, contents);
                }
                sd_bus_message_exit_container(m); // variant
            }
            sd_bus_message_exit_container(m); // dict entry
        }
        sd_bus_message_exit_container(m); // properties array
    }

    // Nested children ("av") -- always empty in practice since GetLayout's
    // recursionDepth=1 already stops the server from populating them, but
    // skipped explicitly rather than assumed, since a message must be fully
    // consumed in declared-type order for the following exit_container() calls
    // in the caller to land correctly.
    sd_bus_message_skip(m, "av");
}

bool dbusmenu_fetch(sd_bus *bus, const char *bus_name, const char *menu_path,
                     DbusMenuEntry *out_entries, int *out_count) {
    *out_count = 0;

    // Belt-and-suspenders: some DBusMenu implementations lazily populate a
    // menu's contents only once AboutToShow() has been called on its root (id 0).
    // Its needUpdate return value isn't acted on -- GetLayout is always called
    // next regardless, and the live item tested during development returned a
    // complete menu without this call ever mattering, but it's cheap and
    // spec-conventional to send anyway.
    sd_bus_error about_err = SD_BUS_ERROR_NULL;
    sd_bus_message *about_reply = NULL;
    sd_bus_call_method(bus, bus_name, menu_path, MENU_INTERFACE, "AboutToShow",
                        &about_err, &about_reply, "i", (int32_t)0);
    sd_bus_error_free(&about_err);
    if (about_reply) {
        sd_bus_message_unref(about_reply);
    }

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    // parentId=0 (root), recursionDepth=1 (direct children only -- matches the
    // reference's flat, non-recursive rendering), propertyNames=[] (all
    // properties; passing none means "no filter", i.e. everything).
    int rc = sd_bus_call_method(bus, bus_name, menu_path, MENU_INTERFACE, "GetLayout",
                                 &err, &reply, "iias", (int32_t)0, (int32_t)1, 0);
    if (rc < 0) {
        fprintf(stderr, "stickBar: GetLayout failed for %s%s (%s)\n", bus_name, menu_path,
                err.message ? err.message : "unknown error");
        sd_bus_error_free(&err);
        return false;
    }

    // Reply shape "u(ia{sv}av)": two separate top-level values -- a plain u32
    // revision, then the root item's own (id, properties, children) tuple.
    uint32_t revision = 0;
    sd_bus_message_read_basic(reply, SD_BUS_TYPE_UINT32, &revision);
    (void)revision; // not needed -- this isn't a live-update-tracking client

    if (sd_bus_message_enter_container(reply, SD_BUS_TYPE_STRUCT, "ia{sv}av") > 0) {
        int32_t root_id = 0;
        sd_bus_message_read_basic(reply, SD_BUS_TYPE_INT32, &root_id);
        sd_bus_message_skip(reply, "a{sv}"); // root's own properties -- not needed

        if (sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "v") > 0) {
            while (*out_count < MAX_MENU_ENTRIES &&
                   sd_bus_message_enter_container(reply, SD_BUS_TYPE_VARIANT, "(ia{sv}av)") > 0) {
                if (sd_bus_message_enter_container(reply, SD_BUS_TYPE_STRUCT, "ia{sv}av") > 0) {
                    parse_entry(reply, &out_entries[*out_count]);
                    (*out_count)++;
                    sd_bus_message_exit_container(reply); // child struct
                }
                sd_bus_message_exit_container(reply); // variant
            }
            sd_bus_message_exit_container(reply); // children array
        }
        sd_bus_message_exit_container(reply); // root struct
    }

    sd_bus_message_unref(reply);
    return true;
}

void dbusmenu_trigger(sd_bus *bus, const char *bus_name, const char *menu_path, int entry_id) {
    sd_bus_message *msg = NULL;
    if (sd_bus_message_new_method_call(bus, &msg, bus_name, menu_path, MENU_INTERFACE, "Event") < 0) {
        return;
    }

    // Event(id: i, eventId: s, data: v, timestamp: u). `data` is sent as an
    // empty-string variant (the conventional value real SNI hosts use for a
    // plain click -- the spec only says "arbitrary event data" and doesn't
    // mandate a shape) and `timestamp` as a real Unix time, same time source
    // clock.c already uses elsewhere in this project.
    sd_bus_message_append(msg, "isvu", (int32_t)entry_id, "clicked", "s", "", (uint32_t)time(NULL));

    // Event carries the NoReply annotation -- mark it explicitly so the peer
    // doesn't bother generating a reply this code would just discard anyway.
    sd_bus_message_set_expect_reply(msg, 0);
    sd_bus_send(bus, msg, NULL);
    sd_bus_message_unref(msg);
}
