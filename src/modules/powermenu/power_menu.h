#ifndef CUSTOMBAR_MODULES_POWERMENU_POWER_MENU_H
#define CUSTOMBAR_MODULES_POWERMENU_POWER_MENU_H

#include <stdbool.h>
#include <wayland-client.h>

#include "wayland/context.h"

// Stashes `app` for later use when opening -- the SIGUSR1 signal-handler
// trigger path (main.c, for the Hyprland keybind) has no Output/AppContext
// of its own to hand over, unlike every other popup in this project, which
// is always opened from a click handler that already has one. Same
// "no natural caller-supplied context" problem dbus/sni_registry.c's
// sni_registry_init(AppContext*) and mpris/mpris_client.c's
// mpris_client_init(..., AppContext*) already solve the same way. Call once
// at startup, before power_menu_toggle() can ever be triggered.
void power_menu_init(AppContext *app);

// Opens the full-screen logout/restart/shutdown overlay if it's currently
// closed, or closes it if it's already open -- matches the reference
// PowerMenuState.toggle() semantics exactly. This is the *only* entry point
// (no separate _open()/_close() pair exposed) since both of this bar's own
// triggers -- the Arch logo button (modules/powermenu/arch_logo.c) and the
// SIGUSR1 signal handler main.c wires up for the Hyprland keybind -- always
// want toggle semantics, never an unconditional open.
void power_menu_toggle(void);

bool power_menu_is_menu_surface(struct wl_surface *surface);

// The overlay's own full-screen surface doubles as its own click target --
// unlike every other popup in this project, there's no separate invisible
// click-catcher surface underneath a smaller menu surface, since the
// overlay already covers the whole output by itself. Landing on a button
// triggers that action and closes the menu; landing anywhere else on the
// overlay (the dimmed background) just closes it, matching the reference's
// own full-surface `MouseArea { onClicked: PowerMenuState.close() }`.
void power_menu_handle_click(struct wl_surface *surface, double x, double y);

// Updates which button (if any) is hover-highlighted and redraws if it
// changed, matching PowerButton.qml's own hover state.
void power_menu_handle_motion(struct wl_surface *surface, double x, double y);

#endif
