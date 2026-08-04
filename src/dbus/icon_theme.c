#include "dbus/icon_theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cairo_surface_t *load_and_scale_png(const char *path, int dest_size) {
    cairo_surface_t *src = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(src) != CAIRO_STATUS_SUCCESS) {
        // cairo_image_surface_create_from_png() never returns NULL -- a missing
        // file or a non-PNG at `path` comes back as a valid pointer in an error
        // state, which must be destroyed either way.
        cairo_surface_destroy(src);
        return NULL;
    }

    int w = cairo_image_surface_get_width(src);
    int h = cairo_image_surface_get_height(src);
    if (w <= 0 || h <= 0) {
        cairo_surface_destroy(src);
        return NULL;
    }

    cairo_surface_t *dest = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, dest_size, dest_size);
    if (cairo_surface_status(dest) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(src);
        cairo_surface_destroy(dest);
        return NULL;
    }

    // Same "always scale to a consistent on-bar size" approach as
    // dbus/sni_icon.c's IconPixmap conversion, for the same reason: apps ship
    // whatever sizes they ship, and every tray icon should still end up the
    // same size in the bar.
    cairo_t *cr = cairo_create(dest);
    cairo_scale(cr, (double)dest_size / (double)w, (double)dest_size / (double)h);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_destroy(cr);

    cairo_surface_destroy(src);
    return dest;
}

// Preferred hicolor size directories to probe, largest-reasonable-first --
// downscaling a bigger source generally looks better than upscaling a tiny
// one. Not attempting to be exhaustive (hicolor themes ship many more sizes
// than this); just enough to usually find something reasonable to downscale.
static const char *const HICOLOR_SIZES[] = { "48x48", "64x64", "32x32", "24x24", "16x16" };
#define HICOLOR_SIZE_COUNT (sizeof(HICOLOR_SIZES) / sizeof(HICOLOR_SIZES[0]))

static cairo_surface_t *try_hicolor_base(const char *hicolor_base, const char *icon_name, int dest_size) {
    if (!hicolor_base || !hicolor_base[0]) {
        return NULL;
    }
    char path[512];
    for (size_t i = 0; i < HICOLOR_SIZE_COUNT; i++) {
        snprintf(path, sizeof(path), "%s/%s/apps/%s.png", hicolor_base, HICOLOR_SIZES[i], icon_name);
        cairo_surface_t *surf = load_and_scale_png(path, dest_size);
        if (surf) {
            return surf;
        }
    }
    return NULL;
}

cairo_surface_t *icon_theme_lookup(const char *icon_name, const char *theme_path, int dest_size) {
    if (!icon_name || !icon_name[0]) {
        return NULL;
    }

    char path[512];

    // 1. SNI's IconThemePath override, if the item provided one (confirmed live
    // on this machine: Steam sets it to its own install's icon directory).
    if (theme_path && theme_path[0]) {
        snprintf(path, sizeof(path), "%s/%s.png", theme_path, icon_name);
        cairo_surface_t *surf = load_and_scale_png(path, dest_size);
        if (surf) {
            return surf;
        }
    }

    // 2. User-local hicolor theme dir, then the system one -- covers apps
    // installed per-user (e.g. via Flatpak/pip/etc. into $XDG_DATA_HOME) as
    // well as normally-packaged ones.
    const char *xdg_data_home = getenv("XDG_DATA_HOME");
    char user_hicolor[512];
    if (xdg_data_home && xdg_data_home[0]) {
        snprintf(user_hicolor, sizeof(user_hicolor), "%s/icons/hicolor", xdg_data_home);
    } else {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            snprintf(user_hicolor, sizeof(user_hicolor), "%s/.local/share/icons/hicolor", home);
        } else {
            user_hicolor[0] = '\0';
        }
    }

    cairo_surface_t *surf = try_hicolor_base(user_hicolor, icon_name, dest_size);
    if (surf) {
        return surf;
    }
    surf = try_hicolor_base("/usr/share/icons/hicolor", icon_name, dest_size);
    if (surf) {
        return surf;
    }
    surf = try_hicolor_base("/usr/local/share/icons/hicolor", icon_name, dest_size);
    if (surf) {
        return surf;
    }

    // 3. Legacy flat fallback location, still used by a handful of apps.
    snprintf(path, sizeof(path), "/usr/share/pixmaps/%s.png", icon_name);
    return load_and_scale_png(path, dest_size);
}
