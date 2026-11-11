//! `wp_viewporter`: lets a surface show a buffer scaled to a different size.
//!
//! Only the popups' click-catchers use it. A catcher covers a whole monitor
//! but is fully transparent, so instead of a full-size buffer (about 8 MB per
//! 1920x1080 monitor, which is what the C bar first did: BUG.md "Every open
//! dropdown cost ~19 MB") it gets a 1x1 buffer that the viewport stretches
//! to the monitor's size.
//!
//! SCTK has no helper for this protocol, so the bar handles it directly: the
//! global is bound if the compositor offers it, and `ViewporterData` tells
//! SCTK's dispatch machinery (its `Dispatch2` trait) what to do with the
//! protocol's events. Neither interface has any events, so there's nothing to do.

use smithay_client_toolkit::{
    dispatch2::Dispatch2,
    reexports::{
        client::{Connection, QueueHandle, globals::GlobalList},
        protocols::wp::viewporter::client::{
            wp_viewport::{self, WpViewport},
            wp_viewporter::{self, WpViewporter},
        },
    },
};

use super::App;

/// Marker attached to the viewporter objects, routing their (non-existent)
/// events to the handlers below.
#[derive(Debug)]
pub(crate) struct ViewporterData;

impl Dispatch2<WpViewporter, App> for ViewporterData {
    /// `wp_viewporter` sends no events.
    fn event(
        &self,
        _: &mut App,
        _: &WpViewporter,
        _: wp_viewporter::Event,
        _: &Connection,
        _: &QueueHandle<App>,
    ) {
    }
}

impl Dispatch2<WpViewport, App> for ViewporterData {
    /// `wp_viewport` sends no events.
    fn event(
        &self,
        _: &mut App,
        _: &WpViewport,
        _: wp_viewport::Event,
        _: &Connection,
        _: &QueueHandle<App>,
    ) {
    }
}

/// Binds `wp_viewporter` if the compositor has it (Hyprland does). Without it,
/// catchers fall back to full-size transparent buffers.
pub(crate) fn bind(globals: &GlobalList, qh: &QueueHandle<App>) -> Option<WpViewporter> {
    globals.bind(qh, 1..=1, ViewporterData).ok()
}
