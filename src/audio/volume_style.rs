//! How a volume looks, shared by the volume box and (from M9b) both audio
//! dropdowns (the C bar's `audio/volume_style.c`).

use crate::config::glyphs::{
    ICON_VOLUME_HIGH, ICON_VOLUME_LOW, ICON_VOLUME_MEDIUM, ICON_VOLUME_MUTED,
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

#[cfg(test)]
mod tests {
    use super::level_glyph;
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
}
