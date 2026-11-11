//! The bar's widgets, one directory each (the C bar's `modules/`). Each draws
//! its own box and returns where it landed; `render/mod.rs` lays them out.

pub(crate) mod clock;
pub(crate) mod cpu;
pub(crate) mod gpu;
pub(crate) mod mem;
pub(crate) mod mic;
pub(crate) mod network;
pub(crate) mod powermenu;
pub(crate) mod volume;
pub(crate) mod workspaces;
