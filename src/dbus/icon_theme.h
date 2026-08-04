#ifndef CUSTOMBAR_DBUS_ICON_THEME_H
#define CUSTOMBAR_DBUS_ICON_THEME_H

#include <cairo.h>

// Fallback icon lookup for items with no usable IconPixmap (e.g. Steam, which
// advertises only an IconName + IconThemePath). Tries, in order:
//   1. `<theme_path>/<icon_name>.png`, if `theme_path` is non-empty -- this is
//      the SNI `IconThemePath` property, a documented per-item override some
//      apps (Steam among them) use to point at their own private icon file
//      instead of relying on the system icon theme.
//   2. A pragmatic, NOT freedesktop-icon-theme-spec-compliant search across
//      hicolor's common size directories and the legacy flat /usr/share/pixmaps
//      -- no index.theme parsing, no theme inheritance chain, no SVG (that
//      would need librsvg or similar, a new dependency this project doesn't
//      have) -- just enough to find a plain PNG most apps' icons ship as.
// Returns NULL if nothing was found or the file wasn't a PNG cairo could
// decode -- callers should fall back to the muted placeholder square.
cairo_surface_t *icon_theme_lookup(const char *icon_name, const char *theme_path, int dest_size);

#endif
