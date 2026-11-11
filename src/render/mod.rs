//! Top-level drawing of one bar, the C bar's `render/render.c`.
//!
//! Each bar module will plug in here from M3 on: left-region modules laid out
//! left to right, right-region ones right to left, the media box centered between.

use crate::config::COLORS;

/// Draws a whole bar into `cr`. For now that's just the background: the
/// Catppuccin mantle fill every module box will sit on.
pub(crate) fn render_output(cr: &cairo::Context) -> Result<(), cairo::Error> {
    COLORS.mantle.set_source(cr);
    cr.paint()
}
