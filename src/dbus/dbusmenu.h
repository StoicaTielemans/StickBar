#ifndef CUSTOMBAR_DBUS_DBUSMENU_H
#define CUSTOMBAR_DBUS_DBUSMENU_H

#include <stdbool.h>
#include <systemd/sd-bus.h>

// Generous fixed bound, matching this project's existing convention over
// dynamic growth (see e.g. modules/workspaces.c's MAX_TRACKED_OUTPUTS).
#define MAX_MENU_ENTRIES 32

typedef struct {
    int id;
    char label[128]; // empty for separators
    bool is_separator;
    bool enabled; // defaults true per the DBusMenu convention when absent from GetLayout
} DbusMenuEntry;

// Fetches the flat, non-recursive (GetLayout recursionDepth=1) top-level menu
// from com.canonical.dbusmenu at (bus_name, menu_path) into `out_entries` (must
// have room for MAX_MENU_ENTRIES), writing the count to *out_count. Returns
// false on any D-Bus failure (*out_count set to 0 in that case).
bool dbusmenu_fetch(sd_bus *bus, const char *bus_name, const char *menu_path,
                     DbusMenuEntry *out_entries, int *out_count);

// Fire-and-forget Event(entry_id, "clicked", "", now). DBusMenu's Event method
// carries the org.freedesktop.DBus.Method.NoReply annotation (confirmed against
// the real com.canonical.dbusmenu interface definition) -- this sends via
// sd_bus_send(), not a reply-waiting call.
void dbusmenu_trigger(sd_bus *bus, const char *bus_name, const char *menu_path, int entry_id);

#endif
