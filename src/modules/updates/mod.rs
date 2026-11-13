//! The pending-updates box (`docs/modules/updates.md`; the data is
//! `src/updates/`): `<package icon> 54` in pink, just left of the mic box.
//! Only shown while there's something to update, like the privacy box, so an
//! up-to-date system costs no bar space. A right click lists the packages
//! (`popup.rs`); a left click does nothing, at the user's request.

pub(crate) mod popup;

use crate::{
    config::{COLORS, glyphs::ICON_UPDATES},
    render::{
        BoxRect,
        module_box::{self, Anchor},
        text::TextCache,
    },
    updates::Updates,
};

/// Draws the box ending at `right_x` (right region), spanning
/// `top..top + height`; `None` (no box, no space) with nothing to update.
pub(crate) fn render(
    cr: &cairo::Context,
    text: &mut TextCache,
    updates: &Updates,
    right_x: f64,
    (top, height): (f64, f64),
) -> Result<Option<BoxRect>, cairo::Error> {
    let total = updates.total();
    if total == 0 {
        return Ok(None);
    }
    let label = format!("{ICON_UPDATES} {total}");
    module_box::label(
        cr,
        text,
        (Anchor::FromRight, right_x),
        (top, height),
        &label,
        COLORS.pink,
    )
    .map(Some)
}
