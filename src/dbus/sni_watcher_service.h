#ifndef CUSTOMBAR_DBUS_SNI_WATCHER_SERVICE_H
#define CUSTOMBAR_DBUS_SNI_WATCHER_SERVICE_H

#include <stdbool.h>
#include <systemd/sd-bus.h>

// Attempts to become the StatusNotifierWatcher for this session by claiming the
// well-known bus name org.kde.StatusNotifierWatcher and registering the
// /StatusNotifierWatcher object (RegisterStatusNotifierItem/Host, the
// RegisteredStatusNotifierItems/IsStatusNotifierHostRegistered/ProtocolVersion
// properties, and the StatusNotifierItem{,Un}Registered/StatusNotifierHostRegistered
// signals) -- items register directly with this process from then on via
// sni_registry_add(), and a NameOwnerChanged subscription auto-unregisters an
// item whose owning process exits.
//
// Returns true if this process became the Watcher. Returns false (not an error)
// if the bus name is already owned by something else (another bar, or a DE's
// own tray implementation) -- the caller should fall back to
// sni_watcher_register_host() in that case, connecting as a client to whichever
// Watcher does exist instead. sni_registry_init() must already have been called.
bool sni_watcher_service_try_start(sd_bus *bus);

#endif
