#ifndef CUSTOMBAR_CONFIG_GLYPHS_H
#define CUSTOMBAR_CONFIG_GLYPHS_H

#include <stdint.h>

/*
 * Nerd Font icon codepoints used by bar modules. These live in the Supplementary
 * Private Use Area (5-hex-digit codepoints, e.g. 0xF00EE) where hand-typing or
 * pasting the raw UTF-8 bytes is error-prone -- it's easy to end up with a
 * truncated byte sequence plus a stray literal digit. Every codepoint below was
 * verified present in JetBrainsMonoNerdFont-Regular.ttf's cmap via fontTools before
 * being written here (the volume/mic/device-popup ones were additionally derived
 * by reading the exact codepoint back out of the reference Quickshell bar's own
 * QML source via Python's ord(), not retyped by eye). Kept as numeric codepoints
 * and encoded to UTF-8 at runtime by glyph_utf8() rather than embedding raw
 * multi-byte string literals in source.
 */
#define GLYPH_ICON_CALENDAR 0xF00EEu
#define GLYPH_ICON_CLOCK     0xF0954u

// Volume (sink) level icons, matching VolumeWidget.qml's icon() thresholds
// exactly: muted, <34%, <67%, else.
#define GLYPH_ICON_VOLUME_MUTED  0xF0581u
#define GLYPH_ICON_VOLUME_LOW    0xF057Fu
#define GLYPH_ICON_VOLUME_MEDIUM 0xF0580u
#define GLYPH_ICON_VOLUME_HIGH   0xF057Eu

// Mic (source) icons, matching MicWidget.qml's icon().
#define GLYPH_ICON_MIC_MUTED 0xF036Du
#define GLYPH_ICON_MIC       0xF036Cu

// Device-dropdown row icons, matching AudioDevicesPopup.qml/MicDevicesPopup.qml:
// a "default device" marker vs. a plain selectable-circle marker, and the
// speaker-icon prefix used on sink rows (mic rows reuse GLYPH_ICON_MIC above).
#define GLYPH_ICON_DEVICE_DEFAULT     0xF0967u
#define GLYPH_ICON_DEVICE_SELECTABLE  0xF0130u
#define GLYPH_ICON_HEADPHONES         0xF02CBu

// Encodes a single Unicode codepoint as UTF-8 into `out`, nul-terminated.
// `out` must have room for at least 5 bytes (up to 4 bytes of encoding + '\0').
// Returns the number of encoded bytes (excluding the terminator).
int glyph_utf8(uint32_t codepoint, char out[5]);

// Workspace pills render numerals as CJK kanji glyphs instead of digits, matching
// the reference bar. Returns the codepoint for workspace ids 1..10, or 0 for any id
// outside that range (callers should fall back to rendering the plain number).
uint32_t kanji_for_workspace(int id);

#endif
