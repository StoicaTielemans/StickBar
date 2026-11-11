//! Monitors coming and going (the C bar's `wayland/output.c`).
//!
//! SCTK collects each `wl_output`'s details (name, mode, ...) and calls
//! `new_output` once they're complete, the moment the C bar handled in its
//! `done` listener. Each monitor gets an `Output` record holding its bar.

use smithay_client_toolkit::{
    output::{OutputHandler, OutputState},
    reexports::client::{Connection, QueueHandle, protocol::wl_output::WlOutput},
};

use super::App;
use crate::bar::BarSurface;

/// The environment variable that keeps a test instance on one monitor: every
/// other monitor is ignored entirely (no bar, and later no popup catchers).
const TEST_OUTPUT_VAR: &str = "STICKBAR_TEST_OUTPUT";

/// One tracked monitor (the C bar's `Output`).
pub(crate) struct Output {
    /// The Wayland object for the monitor (named `proxy`, since a field can't
    /// repeat its struct's name under the lint policy).
    proxy: WlOutput,
    /// 0-based order this monitor was reported in, never reused. The workspaces
    /// module (M5) gives each index its own 5-wide range of workspace ids, and
    /// the tests can name a monitor by it (`rects 0`).
    pub(crate) index: usize,
    /// The connector name, e.g. `DP-2` (empty if the compositor didn't send one).
    pub(crate) name: String,
    /// This monitor's bar; `None` if the compositor closed it.
    pub(crate) bar: Option<BarSurface>,
}

impl Output {
    /// The Wayland object for the monitor.
    pub(crate) const fn wl_output(&self) -> &WlOutput {
        &self.proxy
    }
}

impl OutputHandler for App {
    fn output_state(&mut self) -> &mut OutputState {
        &mut self.output_state
    }

    /// A monitor appeared (at startup, or hotplugged): track it and give it a bar.
    fn new_output(&mut self, _conn: &Connection, qh: &QueueHandle<Self>, output: WlOutput) {
        // The index is taken before the test filter below, so it counts every
        // monitor, exactly as the C bar counted at bind time.
        let index = self.next_output_index;
        self.next_output_index += 1;
        let name = self
            .output_state
            .info(&output)
            .and_then(|info| info.name)
            .unwrap_or_default();

        // A test instance only lives on the monitor it was told to use.
        if let Ok(only) = std::env::var(TEST_OUTPUT_VAR)
            && !only.is_empty()
            && only != name
        {
            return;
        }

        let bar = BarSurface::new(
            qh,
            &self.compositor,
            &self.layer_shell,
            &output,
            self.test_mode,
        );
        self.outputs.push(Output {
            proxy: output,
            index,
            name,
            bar: Some(bar),
        });
    }

    /// A monitor's details changed. Nothing to do: a size change reaches the
    /// bar as a layer-surface configure, and the size is read live when asked.
    fn update_output(&mut self, _conn: &Connection, _qh: &QueueHandle<Self>, _output: WlOutput) {}

    /// A monitor was unplugged: drop its record and bar.
    fn output_destroyed(&mut self, _conn: &Connection, _qh: &QueueHandle<Self>, output: WlOutput) {
        self.outputs.retain(|tracked| tracked.proxy != output);
    }
}
