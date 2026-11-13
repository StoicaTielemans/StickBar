//! The privacy box (the C bar's `modules/privacy/privacy.c`): while an app
//! records the mic, reads a camera or captures the screen, one maroon icon
//! per kind, right after the memory box. With nothing recording it takes no
//! space at all (the user's wish: "when I don't use mic or screenshare I just
//! don't want to see anything"). A right click lists who (`popup.rs`). The
//! data comes from the `pipewire` thread (`src/pipewire/`).

pub(crate) mod popup;

use crate::{
    config::{
        COLORS,
        glyphs::{ICON_MIC, ICON_SCREENSHARE, ICON_WEBCAM},
    },
    pipewire::{Category, PrivacyState},
    render::{
        BoxRect,
        module_box::{self, Anchor},
        text::TextCache,
    },
};

/// The glyph for a kind of recording.
pub(crate) const fn glyph(category: Category) -> char {
    match category {
        Category::Mic => ICON_MIC,
        Category::Webcam => ICON_WEBCAM,
        Category::ScreenShare => ICON_SCREENSHARE,
    }
}

/// The kinds, in the order the box and the dropdown show them.
pub(crate) const CATEGORIES: [Category; 3] =
    [Category::Mic, Category::Webcam, Category::ScreenShare];

/// Draws the box starting at `x` (left region), spanning `top..top + height`;
/// `None` (nothing drawn) when nothing records.
pub(crate) fn render(
    cr: &cairo::Context,
    text: &mut TextCache,
    state: &PrivacyState,
    x: f64,
    (top, height): (f64, f64),
) -> Result<Option<BoxRect>, cairo::Error> {
    // One icon per active kind, space-joined: no text, unlike cpu/gpu/mem.
    let icons: Vec<String> = CATEGORIES
        .into_iter()
        .filter(|&c| state.has(c))
        .map(|c| glyph(c).to_string())
        .collect();
    if icons.is_empty() {
        return Ok(None);
    }
    module_box::label(
        cr,
        text,
        (Anchor::FromLeft, x),
        (top, height),
        &icons.join(" "),
        COLORS.maroon,
    )
    .map(Some)
}
