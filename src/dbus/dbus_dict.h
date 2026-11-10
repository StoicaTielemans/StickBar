#ifndef CUSTOMBAR_DBUS_DBUS_DICT_H
#define CUSTOMBAR_DBUS_DBUS_DICT_H

#include <stdbool.h>
#include <stddef.h>
#include <systemd/sd-bus.h>

// Reading the a{sv} property dictionaries every D-Bus protocol here returns
// (MPRIS GetAll/Metadata/PropertiesChanged, SNI GetAll, DBusMenu entry
// properties). Each used to spell out the same enter-array / enter-entry /
// read-key / peek / enter-variant / skip / exit loop.

// Called once per entry, with the message's cursor inside the entry's
// variant; `contents` is the variant's type signature ("s", "b", "a{sv}",
// ...). Read the value and return true, or return false without reading
// anything and the walker skips it. Must never leave a value half-read.
typedef bool (*DbusDictEntryFn)(sd_bus_message *m, const char *key, const char *contents, void *user_data);

// Walks the a{sv} at the cursor, calling `fn` for each entry, and leaves the
// cursor past the whole dictionary. False if there was no a{sv} there.
bool dbus_dict_walk(sd_bus_message *m, DbusDictEntryFn fn, void *user_data);

// For entry callbacks: if `contents` is a string ("s") or object path ("o"),
// reads it into `out` and returns true; otherwise reads nothing and returns
// false (so the walker skips it).
bool dbus_dict_read_string(sd_bus_message *m, const char *contents, char *out, size_t out_size);

#endif
