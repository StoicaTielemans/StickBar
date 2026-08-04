#ifndef CUSTOMBAR_CONFIG_GLYPHS_H
#define CUSTOMBAR_CONFIG_GLYPHS_H

#include <stdint.h>

/*
 * Nerd Font icon codepoints used by bar modules. These live in the Supplementary
 * Private Use Area (5-hex-digit codepoints, e.g. 0xF00EE) where hand-typing or
 * pasting the raw UTF-8 bytes is error-prone -- it's easy to end up with a
 * truncated byte sequence plus a stray literal digit. Both codepoints below were
 * verified present in JetBrainsMonoNerdFont-Regular.ttf's cmap via fontTools before
 * being written here. Kept as numeric codepoints and encoded to UTF-8 at runtime by
 * glyph_utf8() rather than embedding raw multi-byte string literals in source.
 */
#define GLYPH_ICON_CALENDAR 0xF00EEu
#define GLYPH_ICON_CLOCK     0xF0954u

// Encodes a single Unicode codepoint as UTF-8 into `out`, nul-terminated.
// `out` must have room for at least 5 bytes (up to 4 bytes of encoding + '\0').
// Returns the number of encoded bytes (excluding the terminator).
int glyph_utf8(uint32_t codepoint, char out[5]);

// Workspace pills render numerals as CJK kanji glyphs instead of digits, matching
// the reference bar. Returns the codepoint for workspace ids 1..10, or 0 for any id
// outside that range (callers should fall back to rendering the plain number).
uint32_t kanji_for_workspace(int id);

#endif
