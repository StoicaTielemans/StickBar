#include "dbus/dbus_dict.h"

#include <stdio.h>
#include <string.h>

bool dbus_dict_walk(sd_bus_message *m, DbusDictEntryFn fn, void *user_data) {
    if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}") <= 0) {
        return false;
    }
    while (sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv") > 0) {
        const char *key = NULL;
        sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &key);
        char type = 0;
        const char *contents = NULL;
        sd_bus_message_peek_type(m, &type, &contents);
        if (contents && sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, contents) > 0) {
            if (!key || !fn(m, key, contents, user_data)) {
                sd_bus_message_skip(m, contents);
            }
            sd_bus_message_exit_container(m); // variant
        }
        sd_bus_message_exit_container(m); // dict entry
    }
    sd_bus_message_exit_container(m); // array
    return true;
}

bool dbus_dict_read_string(sd_bus_message *m, const char *contents, char *out, size_t out_size) {
    char t = strcmp(contents, "s") == 0 ? SD_BUS_TYPE_STRING : strcmp(contents, "o") == 0 ? SD_BUS_TYPE_OBJECT_PATH : 0;
    if (!t) {
        return false;
    }
    const char *v = NULL;
    sd_bus_message_read_basic(m, t, &v);
    if (v) {
        snprintf(out, out_size, "%s", v);
    }
    return true;
}
