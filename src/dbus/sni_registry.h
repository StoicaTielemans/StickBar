#ifndef CUSTOMBAR_DBUS_SNI_REGISTRY_H
#define CUSTOMBAR_DBUS_SNI_REGISTRY_H

#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>
#include <systemd/sd-bus.h>

#include "wayland/context.h"

typedef struct SniItem SniItem; // opaque

// Stashes `app` for later use by signal callbacks (output_mark_all_dirty() on
// icon changes) -- sd_bus_match_signal callbacks get exactly one fixed userdata
// slot, already claimed per-item by the SniItem* itself, so `app` is read back
// from here instead of being threaded through every callback signature. Call
// once at startup, before sni_watcher_register_host().
void sni_registry_init(AppContext *app);

// Fetches this item's properties (Id, IconPixmap, Menu) via one GetAll call,
// converts its best-matching IconPixmap entry to a cairo surface, and subscribes
// to its signals. No-op if (bus_name, path) is already tracked.
void sni_registry_add(sd_bus *bus, const char *bus_name, const char *path);

// Removes and frees the tracked item matching (bus_name, path), if any.
void sni_registry_remove(const char *bus_name, const char *path);

// Removes and frees every tracked item registered under `bus_name`, regardless
// of path -- used when that bus name's owner disconnects (a single client can
// register more than one item, though rare). Returns the number removed.
int sni_registry_remove_by_bus_name(const char *bus_name);

// Formats item as "<bus_name><path>", the same shape used on the wire by
// RegisteredStatusNotifierItems / StatusNotifierItem{,Un}Registered -- kept
// consistent with sni_watcher.c's split_bus_name_path(), which parses this
// same shape back apart on the client side.
void sni_registry_format_entry(SniItem *item, char *out, size_t out_size);

int sni_registry_count(void);
SniItem *sni_registry_get(int index); // 0 <= index < sni_registry_count()

cairo_surface_t *sni_item_icon_surface(SniItem *item); // NULL -> caller draws a placeholder
bool sni_item_has_menu(SniItem *item);
const char *sni_item_bus_name(SniItem *item);
const char *sni_item_menu_path(SniItem *item);
sd_bus *sni_item_bus(SniItem *item); // for callers issuing their own DBusMenu calls (tray_context_menu.c)

// Fire-and-forget Activate(0, 0).
void sni_item_activate(SniItem *item);

#endif
