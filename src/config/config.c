#include "config/config.h"

// Expands a 0..255 hex triple into a Color literal. The division is a compile-time
// constant expression (both operands are literals), so this is valid in a static
// initializer despite looking like runtime arithmetic.
#define RGB(hex_r, hex_g, hex_b) { (hex_r) / 255.0, (hex_g) / 255.0, (hex_b) / 255.0, 1.0 }

const Palette COLORS = {
    .rosewater = RGB(0xf5, 0xe0, 0xdc),
    .flamingo  = RGB(0xf2, 0xcd, 0xcd),
    .pink      = RGB(0xf5, 0xc2, 0xe7),
    .mauve     = RGB(0xcb, 0xa6, 0xf7),
    .red       = RGB(0xf3, 0x8b, 0xa8),
    .maroon    = RGB(0xeb, 0xa0, 0xac),
    .peach     = RGB(0xfa, 0xb3, 0x87),
    .yellow    = RGB(0xf9, 0xe2, 0xaf),
    .green     = RGB(0xa6, 0xe3, 0xa1),
    .teal      = RGB(0x94, 0xe2, 0xd5),
    .sky       = RGB(0x89, 0xdc, 0xeb),
    .sapphire  = RGB(0x74, 0xc7, 0xec),
    .blue      = RGB(0x89, 0xb4, 0xfa),
    .lavender  = RGB(0xb4, 0xbe, 0xfe),
    .text      = RGB(0xcd, 0xd6, 0xf4),
    .subtext1  = RGB(0xba, 0xc2, 0xde),
    .subtext0  = RGB(0xa6, 0xad, 0xc8),
    .overlay2  = RGB(0x93, 0x99, 0xb2),
    .overlay1  = RGB(0x7f, 0x84, 0x9c),
    .overlay0  = RGB(0x6c, 0x70, 0x86),
    .surface2  = RGB(0x58, 0x5b, 0x70),
    .surface1  = RGB(0x45, 0x47, 0x5a),
    .surface0  = RGB(0x31, 0x32, 0x44),
    .base      = RGB(0x1e, 0x1e, 0x2e),
    .mantle    = RGB(0x18, 0x18, 0x25),
    .crust     = RGB(0x11, 0x11, 0x1b),
};

const BarGeometry BAR = {
    .height = 20,
    .item_spacing = 6,
    .module_border_width = 2,
};

const FontConfig FONT = {
    .family = "JetBrainsMono Nerd Font",
    .xs = 8,
    .sm = 10,     // was 9 -- bumped alongside `label` below so the workspace
                  // pills' kanji numerals scale with the rest of the bar's
                  // now-larger module text instead of looking comparatively tiny
    .md = 11,
    .label = 14,  // was 12 -- the size every bar module box's text renders at
                  // (CPU/GPU/MEM, clock, volume/mic, media, network); bumped
                  // because the previous 12px looked cramped/"blocky" against
                  // BAR.height's 20px. 14px is close to the practical ceiling
                  // here: JetBrainsMono Nerd Font's line height at 14px is
                  // ~18.5px (computed from the font's own hhea ascent/descent
                  // via fontTools), which still fits within the 20px bar
                  // height when vertically centered -- 16px's ~21px line
                  // height would already clip against the bar's own edges.
    .heading = 14,
    .title = 16,
};
