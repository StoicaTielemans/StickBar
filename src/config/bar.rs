//! Bar geometry (the C bar's `BAR`).

/// Bar-wide geometry, the C bar's `BarGeometry`.
#[derive(Debug)]
pub(crate) struct BarGeometry {
    /// Total bar height in px, and the layer surface's exclusive zone (the strip
    /// Hyprland keeps windows out of). `u16` so it converts losslessly to both
    /// the `u32` and the `i32` the layer-shell requests want.
    pub(crate) height: u16,
    /// Gap in px between neighbouring boxes within a bar region (and between
    /// workspace pills).
    pub(crate) item_spacing: f64,
    /// Default border thickness in px of a bar module box.
    pub(crate) module_border_width: f64,
    /// How much wider in px a bar box is than its label (split evenly, so the
    /// label is centered with half of this on each side).
    pub(crate) module_padding: f64,
}

/// The bar's geometry, the same values as the C bar's `BAR`.
pub(crate) const BAR: BarGeometry = BarGeometry {
    height: 20,
    item_spacing: 6.0,
    module_border_width: 2.0,
    module_padding: 12.0,
};
