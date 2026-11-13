//! The bar's own Wayland surfaces (the C bar's `bar/`): one layer surface per
//! monitor that lives as long as that monitor does, plus the transient popup
//! surfaces dropdowns open and close.

mod bar_surface;
mod damage;
pub(crate) mod popup_host;
pub(crate) mod popup_surface;
pub(crate) mod popups;
pub(crate) mod strip_surface;

pub(crate) use bar_surface::BarSurface;
