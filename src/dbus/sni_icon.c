#include "dbus/sni_icon.h"

#include <arpa/inet.h> // ntohl()
#include <stdbool.h>
#include <string.h>

#include "config/config.h"

// Symbolic/mask-icon detection thresholds (see detect_symbolic_mask() below).
// 40/255 alpha: low-alpha edge/antialiasing pixels are noisy and shouldn't
// count toward the flat-color decision. 12/255 color range: how tightly every
// sampled pixel's RGB must cluster to be considered "one flat color" rather
// than a real multi-color icon. 0.40 luminance: only *dim* flat colors get
// recolored -- a legitimately bright flat-colored icon (rare, but possible)
// already has enough contrast against this bar's dark background and
// shouldn't be touched.
#define SYMBOLIC_ALPHA_THRESHOLD 40
#define SYMBOLIC_COLOR_TOLERANCE 12
#define SYMBOLIC_LUMINANCE_MAX 0.40

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
// **Symbolic/mask icons**: some apps (this includes real, currently-running
// ones -- e.g. easyeffects, whose icon name ends in "-symbolic") ship IconPixmap
// data as a flat-colored *mask* -- every opaque pixel carries the exact same
// dim, low-contrast RGB (verified: literally identical bytes across every fully-
// opaque sampled pixel) -- meant to be recolored at display time to match the
// consuming toolkit's current foreground/theme color. A toolkit doing full
// icon-theme integration (e.g. Qt/KDE, which is how the reference Quickshell bar
// renders bright/legible icons for the exact same SNI items) recolors this mask
// using the IconName property via a real theme lookup instead of trusting the
// pixmap's baked-in color. This project doesn't do theme lookups (see the
// project CLAUDE.md's known limitations), so instead detect_symbolic_mask()
// below applies a cheap heuristic directly on the pixmap bytes: if every
// sufficiently-opaque pixel shares (approximately) one dim color, treat it as a
// mask and recolor it to COLORS.text before premultiplying, leaving alpha (the
// glyph shape) untouched.
static uint32_t unpack_premultiplied(const uint8_t *src, bool recolor) {
    uint32_t argb = ntohl(*(const uint32_t *)src);
    uint8_t a = (uint8_t)(argb >> 24);
    uint8_t r = (uint8_t)(argb >> 16);
    uint8_t g = (uint8_t)(argb >> 8);
    uint8_t b = (uint8_t)argb;

    if (recolor) {
        r = (uint8_t)(COLORS.text.r * 255.0 + 0.5);
        g = (uint8_t)(COLORS.text.g * 255.0 + 0.5);
        b = (uint8_t)(COLORS.text.b * 255.0 + 0.5);
    }

    r = (uint8_t)((unsigned)r * a / 255);
    g = (uint8_t)((unsigned)g * a / 255);
    b = (uint8_t)((unsigned)b * a / 255);

    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// First pass over the raw (pre-premultiply) bytes: decides whether this pixmap
// looks like a flat-colored mask rather than a real multi-color icon. Samples
// only pixels above SYMBOLIC_ALPHA_THRESHOLD (edge/antialiasing pixels near
// alpha=0 are noisy and shouldn't count), then checks both that every sampled
// pixel's RGB clusters within SYMBOLIC_COLOR_TOLERANCE of each other *and* that
// the shared color is dim (below SYMBOLIC_LUMINANCE_MAX) -- a bright flat color
// is rare but already legible on a dark bar and shouldn't be overridden.
static bool detect_symbolic_mask(const uint8_t *pixels, int width, int height) {
    uint8_t min_r = 255, min_g = 255, min_b = 255;
    uint8_t max_r = 0, max_g = 0, max_b = 0;
    bool any_sampled = false;

    for (int y = 0; y < height; y++) {
        const uint8_t *row = pixels + (size_t)y * (size_t)width * 4;
        for (int x = 0; x < width; x++) {
            uint32_t argb = ntohl(*(const uint32_t *)(row + (size_t)x * 4));
            uint8_t a = (uint8_t)(argb >> 24);
            if (a <= SYMBOLIC_ALPHA_THRESHOLD) {
                continue;
            }
            uint8_t r = (uint8_t)(argb >> 16);
            uint8_t g = (uint8_t)(argb >> 8);
            uint8_t b = (uint8_t)argb;
            any_sampled = true;
            if (r < min_r) min_r = r;
            if (g < min_g) min_g = g;
            if (b < min_b) min_b = b;
            if (r > max_r) max_r = r;
            if (g > max_g) max_g = g;
            if (b > max_b) max_b = b;
        }
    }

    if (!any_sampled) {
        return false;
    }

    int range = max_r - min_r;
    if (max_g - min_g > range) range = max_g - min_g;
    if (max_b - min_b > range) range = max_b - min_b;
    if (range > SYMBOLIC_COLOR_TOLERANCE) {
        return false;
    }

    // Rec. 601 luma of the shared color's midpoint, normalized to 0..1.
    double luminance = (0.299 * (max_r + min_r) / 2.0 + 0.587 * (max_g + min_g) / 2.0 +
                         0.114 * (max_b + min_b) / 2.0) / 255.0;
    return luminance < SYMBOLIC_LUMINANCE_MAX;
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

    bool recolor = detect_symbolic_mask(pixels, width, height);

    uint8_t *dst_data = cairo_image_surface_get_data(src_surface);
    int dst_stride = cairo_image_surface_get_stride(src_surface);

    for (int y = 0; y < height; y++) {
        const uint8_t *src_row = pixels + (size_t)y * (size_t)width * 4;
        uint32_t *dst_row = (uint32_t *)(dst_data + (size_t)y * (size_t)dst_stride);
        for (int x = 0; x < width; x++) {
            dst_row[x] = unpack_premultiplied(src_row + (size_t)x * 4, recolor);
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
