//! How a volume looks, shared by the volume box and the audio dropdowns
//! (the C bar's `audio/volume_style.c`).

use crate::{
    config::{
        COLORS, Color,
        glyphs::{ICON_VOLUME_HIGH, ICON_VOLUME_LOW, ICON_VOLUME_MEDIUM, ICON_VOLUME_MUTED},
    },
    render::whole_px,
};

/// The speaker icon for a level, with the reference bar's thresholds: muted
/// (or 0 %), under 34 %, under 67 %, else high.
pub(crate) const fn level_glyph(percent: u32, muted: bool) -> char {
    match percent {
        _ if muted => ICON_VOLUME_MUTED,
        0 => ICON_VOLUME_MUTED,
        1..34 => ICON_VOLUME_LOW,
        34..67 => ICON_VOLUME_MEDIUM,
        _ => ICON_VOLUME_HIGH,
    }
}

/// A volume meter's three colour stops: all `overlay0` when muted;
/// otherwise the accent, with the full end turning peach past 100 % (the
/// meters span 0-100, so a full meter with a peach end reads as "boosted").
pub(crate) const fn meter_gradient(accent: Color, muted: bool, percent: u32) -> [Color; 3] {
    if muted {
        return [COLORS.overlay0; 3];
    }
    [
        accent,
        accent,
        if percent > 100 { COLORS.peach } else { accent },
    ]
}

/// The percent a pointer at `x` sets on a slider over `track_x..track_x +
/// track_w` running 0..=`max`, clamped to the ends (a drag can go far past
/// them under Wayland's implicit grab).
pub(crate) fn slider_percent(x: f64, (track_x, track_w): (f64, f64), max: u32) -> u32 {
    let fraction = ((x - track_x) / track_w).clamp(0.0, 1.0);
    // In range after the clamp, so the conversion can't fail.
    u32::try_from(whole_px((fraction * f64::from(max)).round())).unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::{level_glyph, slider_percent};
    use crate::config::glyphs::{
        ICON_VOLUME_HIGH, ICON_VOLUME_LOW, ICON_VOLUME_MEDIUM, ICON_VOLUME_MUTED,
    };

    #[test]
    fn thresholds() {
        assert_eq!(level_glyph(80, true), ICON_VOLUME_MUTED);
        assert_eq!(level_glyph(0, false), ICON_VOLUME_MUTED);
        assert_eq!(level_glyph(33, false), ICON_VOLUME_LOW);
        assert_eq!(level_glyph(34, false), ICON_VOLUME_MEDIUM);
        assert_eq!(level_glyph(66, false), ICON_VOLUME_MEDIUM);
        assert_eq!(level_glyph(67, false), ICON_VOLUME_HIGH);
        assert_eq!(level_glyph(150, false), ICON_VOLUME_HIGH);
    }

    #[test]
    fn slider_clamps_and_rounds() {
        assert_eq!(slider_percent(50.0, (0.0, 200.0), 100), 25);
        assert_eq!(slider_percent(-30.0, (0.0, 200.0), 100), 0);
        assert_eq!(slider_percent(999.0, (0.0, 200.0), 100), 100);
        assert_eq!(slider_percent(101.0, (0.0, 200.0), 100), 51); // 50.5 rounds up
    }
}
