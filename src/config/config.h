#ifndef CUSTOMBAR_CONFIG_CONFIG_H
#define CUSTOMBAR_CONFIG_CONFIG_H

/*
 * Central styling/geometry constants, ported from the reference Quickshell bar's
 * config.js. Values genuinely shared across modules live here; a widget's own
 * one-off tuning (e.g. the workspace pill's padding/border) stays local to that
 * widget's module instead, matching the convention documented in the reference
 * project.
 */

// cairo wants color channels as 0..1 doubles, not 0..255 ints.
typedef struct {
    double r, g, b, a;
} Color;

// Full Catppuccin Mocha palette. Only a subset (mantle, pink, lavender, base, mauve,
// text) is used by the workspace/clock modules in this first pass, but the whole
// palette is ported up front since every later module (volume, network, tray, ...)
// picks its own accent color from it, same as the QML original.
typedef struct {
    Color rosewater, flamingo, pink, mauve, red, maroon, peach, yellow, green, teal,
          sky, sapphire, blue, lavender, text, subtext1, subtext0, overlay2, overlay1,
          overlay0, surface2, surface1, surface0, base, mantle, crust;
} Palette;

extern const Palette COLORS;

typedef struct {
    int height;               // px, total bar height and the layer-surface's exclusive zone
    int item_spacing;         // px, gap between items within a bar region (left/center/right)
    int module_border_width;  // px, default border thickness on bar module boxes
} BarGeometry;

extern const BarGeometry BAR;

typedef struct {
    const char *family;
    int xs;      // px, fine print -- e.g. the clock's year-calendar day numbers
    int sm;      // px, workspace numeral text size / weekday initials
    int md;      // px, secondary emphasis -- e.g. the year-calendar's month names
    int label;   // px, default bar module text size
    int heading; // px, the audio device popup's default/mute marker icon column,
                 // matching the reference config.js's font.sizes.heading
    int title;   // px, large title text -- e.g. the year-calendar's year number
} FontConfig;

extern const FontConfig FONT;

#endif
