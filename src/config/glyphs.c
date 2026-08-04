#include "config/glyphs.h"

int glyph_utf8(uint32_t cp, char out[5]) {
    // Standard UTF-8 encoding by codepoint range. The PUA icon codepoints used here
    // are all in the 4-byte range (>= 0x10000); the kanji numerals are 3-byte
    // (BMP, >= 0x0800). All ranges are implemented for correctness/reuse.
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        out[1] = '\0';
        return 1;
    } else if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        out[2] = '\0';
        return 2;
    } else if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        out[3] = '\0';
        return 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        out[4] = '\0';
        return 4;
    }
}

// Codepoints for CJK ideographs "one" through "ten", verified via Python ord()
// against the reference Quickshell config's kanjiMap rather than hand-copied.
static const uint32_t KANJI_1_TO_10[10] = {
    0x4E00, // 一 1
    0x4E8C, // 二 2
    0x4E09, // 三 3
    0x56DB, // 四 4
    0x4E94, // 五 5
    0x516D, // 六 6
    0x4E03, // 七 7
    0x516B, // 八 8
    0x4E5D, // 九 9
    0x5341, // 十 10
};

uint32_t kanji_for_workspace(int id) {
    if (id < 1 || id > 10) {
        return 0;
    }
    return KANJI_1_TO_10[id - 1];
}
