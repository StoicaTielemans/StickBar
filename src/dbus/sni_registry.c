#include "dbus/sni_registry.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/config.h"
#include "dbus/icon_theme.h"
#include "dbus/sni_icon.h"
#include "wayland/output.h"

// Fixed-size, generously above what any real setup would need -- matches this
// project's existing convention (workspaces.c's MAX_TRACKED_OUTPUTS,
// MAX_OCCUPIED_IDS) of a bounded array over dynamic growth. Slots are never
// moved/compacted once handed out (see struct SniItem below for why).
#define MAX_TRAY_ITEMS 32

// Not moved/reordered in memory after creation: a slot's address is handed to
// sd_bus_match_signal() as the callback's fixed userdata pointer, so relocating
// it (e.g. via a compacting array) would leave that subscription pointing at
// stale memory. Removal instead just clears `in_use` and resets the slot in
// place; sni_registry_count()/_get() skip unused slots when iterating.
struct SniItem {
    bool in_use;
    sd_bus *bus;
    char bus_name[256];
    char path[256];
    char id[128];
    char menu_path[256]; // empty or "/" means "no menu" (see sni_item_has_menu)
    char icon_name[128];       // used only as a fallback when IconPixmap is absent
    char icon_theme_path[512]; // per-item IconThemePath override, if the item set one
    cairo_surface_t *icon_surface;
    sd_bus_slot *signal_slot;
};

static struct SniItem g_items[MAX_TRAY_ITEMS];
static AppContext *g_app = NULL;

void sni_registry_init(AppContext *app) {
    g_app = app;
}

static SniItem *find_item(const char *bus_name, const char *path) {
    for (int i = 0; i < MAX_TRAY_ITEMS; i++) {
        if (g_items[i].in_use && strcmp(g_items[i].bus_name, bus_name) == 0 &&
            strcmp(g_items[i].path, path) == 0) {
            return &g_items[i];
        }
    }
    return NULL;
}

static SniItem *find_free_slot(void) {
    for (int i = 0; i < MAX_TRAY_ITEMS; i++) {
        if (!g_items[i].in_use) {
            return &g_items[i];
        }
    }
    return NULL;
}

int sni_registry_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_TRAY_ITEMS; i++) {
        if (g_items[i].in_use) {
            count++;
        }
    }
    return count;
}

SniItem *sni_registry_get(int index) {
    int seen = 0;
    for (int i = 0; i < MAX_TRAY_ITEMS; i++) {
        if (g_items[i].in_use) {
            if (seen == index) {
                return &g_items[i];
            }
            seen++;
        }
    }
    return NULL;
}

cairo_surface_t *sni_item_icon_surface(SniItem *item) {
    return item->icon_surface;
}

bool sni_item_has_menu(SniItem *item) {
    // SNI has no real "null" object-path type; "/" is the conventional sentinel
    // some implementations use for "no menu" when the Menu property is present
    // but not meaningful. An empty string covers implementations that omit the
    // property from GetAll entirely instead.
    return item->menu_path[0] != '\0' && strcmp(item->menu_path, "/") != 0;
}

const char *sni_item_bus_name(SniItem *item) {
    return item->bus_name;
}

const char *sni_item_menu_path(SniItem *item) {
    return item->menu_path;
}

sd_bus *sni_item_bus(SniItem *item) {
    return item->bus;
}

void sni_item_activate(SniItem *item) {
    if (!item->bus) {
        return;
    }
    // Fire-and-forget: Activate() is a normal (reply-expecting) method per the
    // SNI spec, but this is called from a UI click handler running inside the
    // bar's single-threaded event loop -- blocking here waiting for the target
    // app to reply (sd_bus_call_method()) would freeze the whole bar's
    // rendering/input until it does. sd_bus_send() instead queues the call and
    // returns immediately; any eventual reply arrives with no pending-call slot
    // registered to receive it, and sd-bus just discards it silently, which is
    // normal, not an error.
    sd_bus_message *msg = NULL;
    if (sd_bus_message_new_method_call(item->bus, &msg, item->bus_name, item->path,
                                        "org.kde.StatusNotifierItem", "Activate") < 0) {
        return;
    }
    // The (x, y) click-position args are largely vestigial in real
    // implementations; (0, 0) is a conventional, accepted placeholder.
    sd_bus_message_append(msg, "ii", 0, 0);
    sd_bus_send(item->bus, msg, NULL);
    sd_bus_message_unref(msg);
}

// Parses an "a(iiay)" IconPixmap value positioned at the message's current
// read cursor (works whether it arrived via a direct property fetch, already
// unwrapped, or from inside a GetAll dict's variant -- both leave the cursor at
// the same place). Picks the entry whose width is closest to BAR.height (ties
// toward the larger one) and converts just that one; returns NULL if no usable
// entry was present.
static cairo_surface_t *convert_best_pixmap_from_message(sd_bus_message *m) {
    if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "(iiay)") <= 0) {
        return NULL;
    }

    int32_t best_w = 0, best_h = 0;
    const void *best_data = NULL;
    int best_diff = INT_MAX;

    int rc;
    while ((rc = sd_bus_message_enter_container(m, SD_BUS_TYPE_STRUCT, "iiay")) > 0) {
        int32_t w = 0, h = 0;
        sd_bus_message_read_basic(m, SD_BUS_TYPE_INT32, &w);
        sd_bus_message_read_basic(m, SD_BUS_TYPE_INT32, &h);

        const void *data = NULL;
        size_t len = 0;
        sd_bus_message_read_array(m, SD_BUS_TYPE_BYTE, &data, &len);

        if (w > 0 && h > 0 && data && (size_t)w * (size_t)h * 4 <= len) {
            int diff = abs(w - BAR.height);
            if (diff < best_diff || (diff == best_diff && w > best_w)) {
                best_diff = diff;
                best_w = w;
                best_h = h;
                best_data = data;
            }
        }

        sd_bus_message_exit_container(m); // struct
    }
    sd_bus_message_exit_container(m); // array

    if (!best_data) {
        return NULL;
    }
    return sni_icon_convert((const uint8_t *)best_data, best_w, best_h, BAR.height);
}

// Re-fetches just the IconPixmap property (not a full GetAll) and replaces the
// cached surface -- used both by the initial add and by the NewIcon signal
// handler.
static void refresh_icon(SniItem *item) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int rc = sd_bus_get_property(item->bus, item->bus_name, item->path,
                                  "org.kde.StatusNotifierItem", "IconPixmap",
                                  &err, &reply, "a(iiay)");
    if (rc < 0) {
        sd_bus_error_free(&err);
        return;
    }

    cairo_surface_t *surf = convert_best_pixmap_from_message(reply);
    sd_bus_message_unref(reply);
    if (surf) {
        if (item->icon_surface) {
            cairo_surface_destroy(item->icon_surface);
        }
        item->icon_surface = surf;
    }
}

static int on_item_signal(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)ret_error;
    SniItem *item = userdata;
    if (!item->in_use) {
        // Defensive: shouldn't happen, since the slot is unref'd in
        // sni_registry_remove() before the struct is reset.
        return 0;
    }

    const char *member = sd_bus_message_get_member(m);
    if (member && strcmp(member, "NewIcon") == 0) {
        refresh_icon(item);
        if (g_app) {
            output_mark_all_dirty(g_app);
        }
    }
    // NewStatus/NewTitle/NewAttentionIcon/NewOverlayIcon/NewMenu: no-op for v1,
    // matching the reference Tray.qml's own lack of any handling for these
    // beyond whatever Quickshell's SystemTray service does implicitly.
    return 0;
}

static void fetch_item_properties(SniItem *item) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int rc = sd_bus_call_method(item->bus, item->bus_name, item->path,
                                 "org.freedesktop.DBus.Properties", "GetAll",
                                 &err, &reply, "s", "org.kde.StatusNotifierItem");
    if (rc < 0) {
        fprintf(stderr, "customBar: failed to fetch tray item properties for %s%s (%s)\n",
                item->bus_name, item->path, err.message ? err.message : "unknown error");
        sd_bus_error_free(&err);
        return;
    }

    if (sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{sv}") > 0) {
        while (sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "sv") > 0) {
            const char *key = NULL;
            sd_bus_message_read_basic(reply, SD_BUS_TYPE_STRING, &key);

            char type = 0;
            const char *contents = NULL;
            sd_bus_message_peek_type(reply, &type, &contents);

            if (contents && sd_bus_message_enter_container(reply, SD_BUS_TYPE_VARIANT, contents) > 0) {
                if (key && strcmp(key, "Id") == 0 && strcmp(contents, "s") == 0) {
                    const char *id = NULL;
                    sd_bus_message_read_basic(reply, SD_BUS_TYPE_STRING, &id);
                    if (id) {
                        snprintf(item->id, sizeof(item->id), "%s", id);
                    }
                } else if (key && strcmp(key, "Menu") == 0 && strcmp(contents, "o") == 0) {
                    const char *menu = NULL;
                    sd_bus_message_read_basic(reply, SD_BUS_TYPE_OBJECT_PATH, &menu);
                    if (menu) {
                        snprintf(item->menu_path, sizeof(item->menu_path), "%s", menu);
                    }
                } else if (key && strcmp(key, "IconPixmap") == 0 && strcmp(contents, "a(iiay)") == 0) {
                    cairo_surface_t *surf = convert_best_pixmap_from_message(reply);
                    if (surf) {
                        item->icon_surface = surf;
                    }
                } else if (key && strcmp(key, "IconName") == 0 && strcmp(contents, "s") == 0) {
                    const char *icon_name = NULL;
                    sd_bus_message_read_basic(reply, SD_BUS_TYPE_STRING, &icon_name);
                    if (icon_name) {
                        snprintf(item->icon_name, sizeof(item->icon_name), "%s", icon_name);
                    }
                } else if (key && strcmp(key, "IconThemePath") == 0 && strcmp(contents, "s") == 0) {
                    const char *theme_path = NULL;
                    sd_bus_message_read_basic(reply, SD_BUS_TYPE_STRING, &theme_path);
                    if (theme_path) {
                        snprintf(item->icon_theme_path, sizeof(item->icon_theme_path), "%s", theme_path);
                    }
                } else {
                    sd_bus_message_skip(reply, contents);
                }
                sd_bus_message_exit_container(reply); // variant
            }
            sd_bus_message_exit_container(reply); // dict entry
        }
        sd_bus_message_exit_container(reply); // array
    }

    sd_bus_message_unref(reply);

    // No IconPixmap (or it converted to nothing usable) -- fall back to
    // resolving IconName via icon_theme.c (IconThemePath override first, then
    // a pragmatic hicolor/pixmaps filesystem search). Only reached when
    // IconPixmap is genuinely absent, not for items like EasyEffects that
    // *do* have one (see CLAUDE.md's "D-Bus / system tray" section on why that
    // one still renders dim rather than being routed through this fallback).
    if (!item->icon_surface && item->icon_name[0]) {
        item->icon_surface = icon_theme_lookup(item->icon_name, item->icon_theme_path, BAR.height);
    }
}

void sni_registry_add(sd_bus *bus, const char *bus_name, const char *path) {
    if (find_item(bus_name, path)) {
        return; // already tracked
    }
    SniItem *item = find_free_slot();
    if (!item) {
        fprintf(stderr, "customBar: tray item limit (%d) reached, ignoring %s%s\n",
                MAX_TRAY_ITEMS, bus_name, path);
        return;
    }

    memset(item, 0, sizeof(*item));
    item->in_use = true;
    item->bus = bus;
    snprintf(item->bus_name, sizeof(item->bus_name), "%s", bus_name);
    snprintf(item->path, sizeof(item->path), "%s", path);

    fetch_item_properties(item);

    // member=NULL subscribes to every signal from this item, dispatched by name
    // inside on_item_signal() -- simpler than one sd_bus_match_signal() call per
    // signal name, and confirmed via `man sd_bus_match_signal` that a NULL field
    // means "match any" rather than "match nothing".
    sd_bus_match_signal(bus, &item->signal_slot, item->bus_name, item->path,
                         "org.kde.StatusNotifierItem", NULL, on_item_signal, item);

    if (g_app) {
        output_mark_all_dirty(g_app);
    }
}

void sni_registry_remove(const char *bus_name, const char *path) {
    SniItem *item = find_item(bus_name, path);
    if (!item) {
        return;
    }

    if (item->signal_slot) {
        sd_bus_slot_unref(item->signal_slot);
    }
    if (item->icon_surface) {
        cairo_surface_destroy(item->icon_surface);
    }
    memset(item, 0, sizeof(*item)); // in_use = false

    if (g_app) {
        output_mark_all_dirty(g_app);
    }
}

int sni_registry_remove_by_bus_name(const char *bus_name) {
    int removed = 0;
    for (int i = 0; i < MAX_TRAY_ITEMS; i++) {
        if (!g_items[i].in_use || strcmp(g_items[i].bus_name, bus_name) != 0) {
            continue;
        }
        if (g_items[i].signal_slot) {
            sd_bus_slot_unref(g_items[i].signal_slot);
        }
        if (g_items[i].icon_surface) {
            cairo_surface_destroy(g_items[i].icon_surface);
        }
        memset(&g_items[i], 0, sizeof(g_items[i])); // in_use = false
        removed++;
    }

    if (removed > 0 && g_app) {
        output_mark_all_dirty(g_app);
    }
    return removed;
}

void sni_registry_format_entry(SniItem *item, char *out, size_t out_size) {
    snprintf(out, out_size, "%s%s", item->bus_name, item->path);
}
