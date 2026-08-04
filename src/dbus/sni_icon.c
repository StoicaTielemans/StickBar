#include "dbus/sni_icon.h"

#include <arpa/inet.h> // ntohl()
#include <string.h>

// Unpacks one source pixel and returns it as a premultiplied, host-native
// 0xAARRGGBB value ready to store directly into a cairo ARGB32 buffer.
//
// SNI's IconPixmap bytes are documented as ARGB32 in *network byte order*
// (big-endian): for each pixel, memory holds [A, R, G, B] with A at the lowest
// address. ntohl() on that 4-byte span read as a big-endian uint32_t recovers
// exactly the value 0xAARRGGBB in host-native representation -- which is also
// precisely the logical value cairo's CAIRO_FORMAT_ARGB32 wants (cairo just wants
// it as a host-native uint32_t store, so on this little-endian machine the actual
// byte layout ends up reversed from the source, [B,G,R,A] -- but that reversal
// falls out of the plain C store below for free; nothing here manually swaps bytes).
//
// Premultiplication: cairo's ARGB32 format requires premultiplied alpha; SNI's
// IconPixmap is conventionally *not* premultiplied on the wire, so this function
// premultiplies unconditionally. **This was verified against a real running item
// (easyeffects), not just assumed**: dumping the raw wire bytes and this
// function's output side by side confirmed the byte-order unpack recovers a
// coherent icon glyph shape via the alpha channel (a clean equalizer-sliders
// outline, matching what a reference renderer shows for the same item) -- so the
// unpack/premultiply logic itself is correct.
//
// **What *isn't* handled**: some apps (this includes real, currently-running
// ones -- e.g. easyeffects, whose icon name ends in "-symbolic") ship IconPixmap
// data as a flat-colored *mask* -- every opaque pixel carries the exact same
// dim, low-contrast RGB (verified: literally identical bytes across every fully-
// opaque sampled pixel) -- meant to be recolored at display time to match the
// consuming toolkit's current foreground/theme color. A toolkit doing full
// icon-theme integration (e.g. Qt/KDE, which is how the reference Quickshell bar
// renders bright/legible icons for the exact same SNI items) recolors this mask
// using the IconName property via a real theme lookup instead of trusting the
// pixmap's baked-in color. This project's "IconPixmap only, no theme lookup"
// scope (see the project CLAUDE.md's known limitations) means symbolic-icon
// apps render in whatever flat color they happened to bake in -- which can look
// low-contrast against a dark bar -- rather than a placeholder square (that
// fallback is only for items with *no* IconPixmap at all, e.g. Steam).
static uint32_t unpack_premultiplied(const uint8_t *src) {
    uint32_t argb = ntohl(*(const uint32_t *)src);
    uint8_t a = (uint8_t)(argb >> 24);
    uint8_t r = (uint8_t)(argb >> 16);
    uint8_t g = (uint8_t)(argb >> 8);
    uint8_t b = (uint8_t)argb;

    r = (uint8_t)((unsigned)r * a / 255);
    g = (uint8_t)((unsigned)g * a / 255);
    b = (uint8_t)((unsigned)b * a / 255);

    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

cairo_surface_t *sni_icon_convert(const uint8_t *pixels, int width, int height, int dest_size) {
    cairo_surface_t *src_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(src_surface) != CAIRO_STATUS_SUCCESS) {
        // cairo never returns NULL from a surface-create call -- a failed
        // allocation is instead represented as a valid pointer in an error
        // state, which must be checked explicitly and still destroyed.
        cairo_surface_destroy(src_surface);
        return NULL;
    }

    uint8_t *dst_data = cairo_image_surface_get_data(src_surface);
    int dst_stride = cairo_image_surface_get_stride(src_surface);

    for (int y = 0; y < height; y++) {
        const uint8_t *src_row = pixels + (size_t)y * (size_t)width * 4;
        uint32_t *dst_row = (uint32_t *)(dst_data + (size_t)y * (size_t)dst_stride);
        for (int x = 0; x < width; x++) {
            dst_row[x] = unpack_premultiplied(src_row + (size_t)x * 4);
        }
    }
    // Required after writing into the surface's data buffer directly -- cairo has
    // no way to know the pixels changed otherwise, and will otherwise use stale
    // (or undefined, for a freshly-created surface) cached content the next time
    // this surface is used as a paint source.
    cairo_surface_mark_dirty(src_surface);

    cairo_surface_t *dest_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, dest_size, dest_size);
    if (cairo_surface_status(dest_surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(src_surface);
        cairo_surface_destroy(dest_surface);
        return NULL;
    }

    // Always scale to the destination size regardless of the source's exact
    // dimensions, via cairo's own geometric scaling (not an image-decode
    // library -- this doesn't conflict with the "IconPixmap only, no decode lib"
    // scope decision). Guarantees every tray icon renders at a consistent size
    // regardless of which pixmap sizes a given app happens to offer.
    cairo_t *cr = cairo_create(dest_surface);
    cairo_scale(cr, (double)dest_size / (double)width, (double)dest_size / (double)height);
    cairo_set_source_surface(cr, src_surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_destroy(cr);

    cairo_surface_destroy(src_surface);
    return dest_surface;
}
