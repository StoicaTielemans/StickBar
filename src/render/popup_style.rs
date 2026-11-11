//! Layout tokens every dropdown shares (the C bar's `render/popup_style.h`):
//! the header/pill/card rhythm all of them were restyled to (`docs/popups.md`,
//! "Look"). A popup whose value really differs (the calendar's wider margin)
//! keeps its own local constant, saying why.

/// Popup edge to content.
pub(crate) const OUTER_MARGIN: f64 = 12.0;
/// Header to the first card, and between sections.
pub(crate) const SECTION_GAP: f64 = 10.0;
/// Between stacked or side-by-side cards.
pub(crate) const CARD_GAP: f64 = 8.0;
/// Between columns in a row (e.g. header text and its pill).
pub(crate) const COL_GAP: f64 = 10.0;
/// The header row's height.
pub(crate) const HEADER_H: f64 = 22.0;
/// Header icon to title.
pub(crate) const HEADER_ICON_GAP: f64 = 8.0;
/// A status pill's height.
pub(crate) const PILL_H: f64 = 16.0;
/// A status pill's horizontal padding, each side.
pub(crate) const PILL_PAD_X: f64 = 7.0;
