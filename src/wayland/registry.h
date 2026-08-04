#ifndef CUSTOMBAR_WAYLAND_REGISTRY_H
#define CUSTOMBAR_WAYLAND_REGISTRY_H

#include "wayland/context.h"

// Connects to the Wayland display, binds the registry, and performs the roundtrips
// needed so every currently-advertised global (compositor, shm, seat, layer_shell,
// and any outputs already present) is bound and initialized before this returns.
// Returns false on failure (a message is printed to stderr).
bool wayland_connect(AppContext *app);

#endif
