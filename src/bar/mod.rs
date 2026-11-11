//! The bar's own Wayland surfaces: one layer surface per monitor that lives as
//! long as that monitor does (the C bar's `bar/`).

mod bar_surface;
mod damage;

pub(crate) use bar_surface::BarSurface;
