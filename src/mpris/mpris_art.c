#include "mpris/mpris_art.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <stdint.h>
#include <string.h>

// cairo's CAIRO_FORMAT_ARGB32 wants premultiplied alpha stored as a
// host-native 0xAARRGGBB uint32_t -- the same target format
// dbus/sni_icon.c's own IconPixmap conversion produces, just starting from a
// GdkPixbuf's row-major, non-premultiplied RGB(A) buffer instead of SNI's
// network-byte-order ARGB wire bytes. Premultiplication is applied
// unconditionally, same reasoning sni_icon.c already documents for its own
// conversion.
static cairo_surface_t *pixbuf_to_cairo(GdkPixbuf *pixbuf) {
    int w = gdk_pixbuf_get_width(pixbuf);
    int h = gdk_pixbuf_get_height(pixbuf);
    int channels = gdk_pixbuf_get_n_channels(pixbuf);
    int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    const guchar *pixels = gdk_pixbuf_get_pixels(pixbuf);

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return NULL;
    }

    unsigned char *dst = cairo_image_surface_get_data(surf);
    int dst_stride = cairo_image_surface_get_stride(surf);

    for (int y = 0; y < h; y++) {
        const guchar *src_row = pixels + (size_t)y * (size_t)rowstride;
        uint32_t *dst_row = (uint32_t *)(dst + (size_t)y * (size_t)dst_stride);
        for (int x = 0; x < w; x++) {
            const guchar *px = src_row + (size_t)x * (size_t)channels;
            uint32_t r = px[0], g = px[1], b = px[2];
            uint32_t a = channels >= 4 ? px[3] : 255u;
            r = r * a / 255u;
            g = g * a / 255u;
            b = b * a / 255u;
            dst_row[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    cairo_surface_mark_dirty(surf);
    return surf;
}

cairo_surface_t *mpris_art_load(const char *art_url, int target_size) {
    if (!art_url || !art_url[0]) {
        return NULL;
    }
    // Only local file:// art is supported -- most desktop players that cache
    // art locally (mpv, VLC, Firefox's own cached media thumbnails) report
    // it this way, but streaming services (Spotify's own cover-art CDN
    // URLs, most in-browser media) hand back an http(s) URL this project has
    // no HTTP client to fetch. A deliberate, documented scope decision (see
    // CLAUDE.md's "Media (MPRIS)" section), not an oversight -- those
    // players just show the popup's placeholder square instead.
    if (strncmp(art_url, "file://", 7) != 0) {
        return NULL;
    }

    // g_filename_from_uri() (not hand-rolled percent-decoding) handles a
    // file:// URI's percent-encoded path correctly, including the edge
    // cases (spaces, unicode filenames) a naive strncpy-past-the-prefix
    // wouldn't.
    GError *err = NULL;
    char *path = g_filename_from_uri(art_url, NULL, &err);
    if (!path) {
        if (err) {
            g_error_free(err);
        }
        return NULL;
    }

    // gdk-pixbuf's loader plugins cover PNG/JPEG/etc. -- cairo alone only
    // decodes PNG natively, which would miss most real-world cover art
    // (commonly JPEG). Scaling at load time (rather than loading full-size
    // and scaling in cairo) keeps this cheap even for a large source image.
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(path, target_size, target_size, FALSE, &err);
    g_free(path);
    if (!pixbuf) {
        if (err) {
            g_error_free(err);
        }
        return NULL;
    }

    cairo_surface_t *surf = pixbuf_to_cairo(pixbuf);
    g_object_unref(pixbuf);
    return surf;
}
