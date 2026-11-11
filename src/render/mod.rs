//! Drawing one bar (the C bar's `render/render.c`).
//!
//! The bar has three regions, as in the C bar: left-region modules laid out
//! left to right from x = 0, right-region ones right to left from the right
//! edge, and the media box centered between. So far only the clock exists, at
//! the right edge.

pub(crate) mod module_box;
pub(crate) mod text;

use crate::{config::COLORS, modules::clock::Clock};
use text::TextCache;

/// A rectangle in bar-surface pixels (cairo works in `f64`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct BoxRect {
    /// Left edge.
    pub(crate) x: f64,
    /// Top edge.
    pub(crate) y: f64,
    /// Width.
    pub(crate) w: f64,
    /// Height.
    pub(crate) h: f64,
}

/// Where one module's box was drawn. Each bar keeps the list from its last full
/// redraw: the tests read it (`rects`), and from M4 clicks are hit-tested against it.
#[derive(Clone, Copy, Debug)]
pub(crate) struct ModuleBox {
    /// The module's name, as the tests know it (`clock`, `cpu`, ...).
    pub(crate) name: &'static str,
    /// Where its box is.
    pub(crate) rect: BoxRect,
}

/// The drawing state every bar shares: the text cache and each module's data.
/// One instance, owned by `App`, used by every monitor's bar.
#[derive(Debug)]
pub(crate) struct Renderer {
    /// Shaped-text cache (pango).
    text: TextCache,
    /// The clock module.
    pub(crate) clock: Clock,
}

impl Renderer {
    /// Sets up pango and the modules.
    pub(crate) fn new() -> Result<Self, cairo::Error> {
        Ok(Self {
            text: TextCache::new()?,
            clock: Clock::new(),
        })
    }

    /// Draws a whole bar `width` x `height` into `cr` and returns where each
    /// module's box landed.
    pub(crate) fn render_output(
        &mut self,
        cr: &cairo::Context,
        width: f64,
        height: f64,
    ) -> Result<Vec<ModuleBox>, cairo::Error> {
        COLORS.mantle.set_source(cr);
        cr.paint()?;

        // Right region, laid out from the right edge leftwards.
        let clock = self
            .clock
            .render(cr, &mut self.text, width, (0.0, height))?;
        Ok(vec![ModuleBox {
            name: "clock",
            rect: clock,
        }])
    }

    /// Repaints only the clock, over its previous box `old`, on a frame that
    /// otherwise already holds the previous picture (C's
    /// `render_output_clock_only`). The clock ticks far more often than anything
    /// else changes, so this skips laying out every other module.
    ///
    /// Returns the new box, or `None` if its width changed. Then the boxes to
    /// its left would have to move, so the caller must do a full redraw instead.
    pub(crate) fn render_clock_only(
        &mut self,
        cr: &cairo::Context,
        old: BoxRect,
    ) -> Result<Option<BoxRect>, cairo::Error> {
        cr.save()?;
        cr.rectangle(old.x, old.y, old.w, old.h);
        cr.clip();
        COLORS.mantle.set_source(cr);
        cr.paint()?;
        let new = self
            .clock
            .render(cr, &mut self.text, old.x + old.w, (old.y, old.h))?;
        cr.restore()?;
        // Widths are whole pixels (pango pixel sizes plus fixed padding), so a
        // half-pixel tolerance is an exact comparison without float `==`.
        Ok(((new.w - old.w).abs() < 0.5).then_some(new))
    }
}
