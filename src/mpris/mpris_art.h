#ifndef CUSTOMBAR_MPRIS_MPRIS_ART_H
#define CUSTOMBAR_MPRIS_MPRIS_ART_H

#include <cairo.h>

// Converts an MPRIS mpris:artUrl into a target_size x target_size cairo
// surface (scaled to fit, aspect ratio not preserved -- callers draw it into
// a fixed square thumbnail slot), or NULL if the URL is missing, uses an
// unsupported scheme, or fails to load/decode. Only file:// URIs are
// supported -- see mpris_art.c's own doc comment for why http(s)/data: art
// (e.g. Spotify's own cover-art CDN URLs) isn't fetched. Pure conversion, no
// D-Bus types -- same "no D-Bus types in this file, deliberately" convention
// dbus/sni_icon.c already documents for the SNI-domain equivalent of this
// file.
cairo_surface_t *mpris_art_load(const char *art_url, int target_size);

#endif
