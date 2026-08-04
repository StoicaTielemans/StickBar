#ifndef CUSTOMBAR_DBUS_SNI_ICON_H
#define CUSTOMBAR_DBUS_SNI_ICON_H

#include <cairo.h>
#include <stdint.h>

// Converts one already-selected StatusNotifierItem IconPixmap entry (raw ARGB32
// bytes in network byte order, `width`x`height`, exactly `width*height*4` tightly
// packed bytes at `pixels` -- no row padding, per the SNI wire format) into a
// cairo_surface_t* scaled to exactly `dest_size`x`dest_size`. Returns NULL on
// failure. Pure function -- no D-Bus types -- see sni_icon.c for the byte-order
// and premultiplication handling this performs, including the single riskiest
// unverified assumption in the whole tray feature (whether source pixels are
// premultiplied), and for the flat-colored-mask ("symbolic" icon) detection
// heuristic that recolors such icons to this bar's own text color instead of
// rendering their dim baked-in color as-is.
cairo_surface_t *sni_icon_convert(const uint8_t *pixels, int width, int height, int dest_size);

#endif
