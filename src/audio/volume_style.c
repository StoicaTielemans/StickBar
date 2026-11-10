#include "audio/volume_style.h"

#include <math.h>

#include "config/glyphs.h"

uint32_t volume_level_glyph(int percent, bool muted) {
    if (muted || percent == 0) return GLYPH_ICON_VOLUME_MUTED;
    if (percent < 34) return GLYPH_ICON_VOLUME_LOW;
    if (percent < 67) return GLYPH_ICON_VOLUME_MEDIUM;
    return GLYPH_ICON_VOLUME_HIGH;
}

void volume_meter_gradient(Color out[3], Color accent, bool muted, int percent) {
    if (muted) {
        out[0] = out[1] = out[2] = COLORS.overlay0;
        return;
    }
    out[0] = out[1] = accent;
    out[2] = percent > 100 ? COLORS.peach : accent;
}

int volume_slider_percent(double x, double track_x, double track_w, int max_percent) {
    double frac = (x - track_x) / track_w;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    return (int)lround(frac * max_percent);
}
