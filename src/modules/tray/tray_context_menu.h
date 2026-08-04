#ifndef CUSTOMBAR_MODULES_TRAY_TRAY_CONTEXT_MENU_H
#define CUSTOMBAR_MODULES_TRAY_TRAY_CONTEXT_MENU_H

#include <stdbool.h>
#include <wayland-client.h>

#include "dbus/sni_registry.h"
#include "wayland/context.h"

// Opens the context menu for `item`, positioned relative to its icon's rect
// (icon_x, icon_y, icon_w, icon_h) on `output` -- see tray_context_menu.c for
// the anchor+margin math. Closes any previously open menu first (at most one
// menu open at a time, a deliberate v1 simplification -- this module's state is
// a single static singleton, not per-item).
void tray_context_menu_open(Output *output, SniItem *item, double icon_x, double icon_y, double icon_w, double icon_h);

// Closes the currently open menu, if any (no-op otherwise).
void tray_context_menu_close(void);

// True if `surface` is the menu's own popup surface (as opposed to one of its
// per-output click-catcher surfaces) -- used by wayland/pointer.c to route
// enter/motion/button events to hover-highlight/click-to-trigger handling.
bool tray_context_menu_is_menu_surface(struct wl_surface *surface);

// True if `surface` is one of the menu's full-screen click-catcher surfaces --
// used by wayland/pointer.c: any button press landing on one closes the menu.
bool tray_context_menu_is_catcher_surface(struct wl_surface *surface);

// Hover tracking within the open menu (updates which entry is highlighted and
// redraws if it changed). No-op if no menu is currently open.
void tray_context_menu_handle_motion(double x, double y);

// Click-to-trigger-then-close within the open menu. No-op if no menu is open or
// (x, y) doesn't land on an enabled entry.
void tray_context_menu_handle_click(double x, double y);

#endif
