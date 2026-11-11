//! Bar geometry. Only what the empty bar uses so far: spacing, border width and
//! padding arrive with the first module that draws a box (M3, the clock).

/// Bar-wide geometry, the C bar's `BarGeometry`.
#[derive(Debug)]
pub(crate) struct BarGeometry {
    /// Total bar height in px, and the layer surface's exclusive zone (the strip
    /// Hyprland keeps windows out of). `u16` so it converts losslessly to both
    /// the `u32` and the `i32` the layer-shell requests want.
    pub(crate) height: u16,
}

/// The bar's geometry, the same values as the C bar's `BAR`.
pub(crate) const BAR: BarGeometry = BarGeometry { height: 20 };
