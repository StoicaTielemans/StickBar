//! Monitors coming and going (the C bar's `wayland/output.c`).
//!
//! SCTK collects each `wl_output`'s details (name, mode, ...) and calls
//! `new_output` once they're complete, the moment the C bar handled in its
//! `done` listener. Each monitor gets one bar, dropped when the monitor goes.
//!
//! Not ported yet: the per-output `index` (binding order) the workspaces module
//! uses for its 5-wide workspace ranges, which lands with workspaces in M5, and the
//! `STICKBAR_TEST_OUTPUT` filter, which lands with the test-control channel in M2.

use smithay_client_toolkit::{
    output::{OutputHandler, OutputState},
    reexports::client::{Connection, QueueHandle, protocol::wl_output::WlOutput},
};

use super::App;
use crate::bar::BarSurface;

impl OutputHandler for App {
    fn output_state(&mut self) -> &mut OutputState {
        &mut self.output_state
    }

    /// A monitor appeared (at startup, or hotplugged): give it a bar.
    fn new_output(&mut self, _conn: &Connection, qh: &QueueHandle<Self>, output: WlOutput) {
        let bar = BarSurface::new(qh, &self.compositor, &self.layer_shell, output);
        self.bars.push(bar);
    }

    /// A monitor's details changed. Nothing to do: a size change reaches the
    /// bar as a layer-surface configure instead.
    fn update_output(&mut self, _conn: &Connection, _qh: &QueueHandle<Self>, _output: WlOutput) {}

    /// A monitor was unplugged: drop its bar.
    fn output_destroyed(&mut self, _conn: &Connection, _qh: &QueueHandle<Self>, output: WlOutput) {
        self.bars.retain(|bar| bar.output() != &output);
    }
}
