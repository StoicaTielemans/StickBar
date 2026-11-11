//! Font settings shared by every module (the C bar's `FONT`). Only the sizes
//! in use so far; the rest (xs, sm, md, heading, title, display) arrive with
//! the popups that use them.

/// Font family and sizes.
#[derive(Debug)]
pub(crate) struct FontConfig {
    /// The one font everything is drawn in. A Nerd Font, so the icon glyphs in
    /// `glyphs.rs` come from the same font as the text.
    pub(crate) family: &'static str,
    /// Bar module box text, in pixels.
    pub(crate) label: i32,
}

/// The font settings, the same values as the C bar's `FONT`.
pub(crate) const FONT: FontConfig = FontConfig {
    family: "JetBrainsMono Nerd Font",
    label: 14,
};
