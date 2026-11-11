//! Font settings shared by every module (the C bar's `FONT`).

/// Font family and sizes.
#[derive(Debug)]
pub(crate) struct FontConfig {
    /// The one font everything is drawn in. A Nerd Font, so the icon glyphs in
    /// `glyphs.rs` come from the same font as the text.
    pub(crate) family: &'static str,
    /// Fine print, e.g. the calendar's ISO week numbers, in pixels.
    pub(crate) xs: i32,
    /// Small text, e.g. the calendar's day numbers and weekday initials.
    pub(crate) sm: i32,
    /// Secondary emphasis, e.g. the calendar's month names.
    pub(crate) md: i32,
    /// Bar module box text.
    pub(crate) label: i32,
    /// A dropdown header's icon column.
    pub(crate) heading: i32,
    /// Large titles, e.g. the calendar's year.
    pub(crate) title: i32,
    /// The power menu's big card icons (the reference uses 40).
    pub(crate) display: i32,
}

/// The font settings, the same values as the C bar's `FONT`.
pub(crate) const FONT: FontConfig = FontConfig {
    family: "JetBrainsMono Nerd Font",
    xs: 8,
    sm: 10,
    md: 11,
    label: 14,
    heading: 14,
    title: 16,
    display: 32,
};
