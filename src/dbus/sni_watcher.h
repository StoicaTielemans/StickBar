#ifndef CUSTOMBAR_DBUS_SNI_WATCHER_H
#define CUSTOMBAR_DBUS_SNI_WATCHER_H

#include <stdbool.h>
#include <systemd/sd-bus.h>

// Registers this process as a StatusNotifierHost with whichever
// StatusNotifierWatcher is actually running, fetches its current item list into
// sni_registry, and subscribes to live item-registered/unregistered signals so
// the tray stays up to date. sni_registry_init() must already have been called.
// Returns false if no Watcher could be found at all -- not fatal, the tray just
// stays empty for this run (mirrors how a missing Hyprland event socket disables
// just the workspace module rather than the whole bar).
bool sni_watcher_register_host(sd_bus *bus);

#endif
