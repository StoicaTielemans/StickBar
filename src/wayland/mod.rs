//! The Wayland side of the bar: the `App` state struct and the handlers the
//! toolkit calls when the compositor sends events (the C bar's `wayland/`).
//!
//! How events reach this code: smithay-client-toolkit (SCTK) reads the Wayland
//! socket, and for each event calls a method of a "handler" trait implemented
//! on `App` below (`CompositorHandler::frame`, `LayerShellHandler::configure`,
//! ...). Every handler gets `&mut App`, so all state lives in that one struct,
//! like the C bar's `AppContext`. The `delegate_*!` macros at the bottom wire
//! the protocol objects to those handlers.

mod output;

use smithay_client_toolkit::{
    compositor::{CompositorHandler, CompositorState},
    delegate_dispatch2, delegate_registry,
    output::OutputState,
    reexports::client::{
        Connection, QueueHandle,
        globals::GlobalList,
        protocol::{wl_output, wl_surface::WlSurface},
    },
    registry::{ProvidesRegistryState, RegistryState},
    registry_handlers,
    shell::{
        WaylandSurface,
        wlr_layer::{LayerShell, LayerShellHandler, LayerSurface, LayerSurfaceConfigure},
    },
    shm::{Shm, ShmHandler},
};

use crate::bar::BarSurface;

/// All of the bar's state, handed to every Wayland event handler.
pub(crate) struct App {
    /// Tracks the compositor's globals (needed by SCTK's registry handling).
    registry_state: RegistryState,
    /// `wl_compositor`: creates surfaces.
    compositor: CompositorState,
    /// `wl_shm`: shared-memory buffers.
    shm: Shm,
    /// Tracks monitors (`wl_output`s) and reports them to `output.rs`.
    output_state: OutputState,
    /// `zwlr_layer_shell_v1`: turns a surface into a bar docked to a screen edge.
    layer_shell: LayerShell,
    /// One bar per monitor.
    bars: Vec<BarSurface>,
    /// Set by SIGINT/SIGTERM: the main loop stops after the current pass.
    pub(crate) exit: bool,
}

impl App {
    /// Binds the globals the bar can't work without. The error is the message
    /// to print, the same one the C bar prints, since the most likely cause is
    /// running under a compositor that isn't wlroots-based.
    pub(crate) fn new(globals: &GlobalList, qh: &QueueHandle<Self>) -> Result<Self, String> {
        let missing = |what: &str| {
            format!(
                "compositor is missing a required Wayland global ({what}) -- \
                 is this running under a wlroots-based compositor?"
            )
        };
        let compositor =
            CompositorState::bind(globals, qh).map_err(|_e| missing("wl_compositor"))?;
        let shm = Shm::bind(globals, qh).map_err(|_e| missing("wl_shm"))?;
        let layer_shell =
            LayerShell::bind(globals, qh).map_err(|_e| missing("zwlr_layer_shell_v1"))?;
        Ok(Self {
            registry_state: RegistryState::new(globals),
            compositor,
            shm,
            output_state: OutputState::new(globals, qh),
            layer_shell,
            bars: Vec::new(),
            exit: false,
        })
    }

    /// Redraws every bar that's dirty and free to draw. The main loop calls this
    /// after each pass, so everything changed in one pass is drawn once.
    pub(crate) fn flush_dirty(&mut self, qh: &QueueHandle<Self>) {
        for bar in &mut self.bars {
            bar.flush(qh);
        }
    }

    /// The bar whose surface is `surface`, if any.
    fn bar_for_surface(&mut self, surface: &WlSurface) -> Option<&mut BarSurface> {
        self.bars.iter_mut().find(|bar| bar.wl_surface() == surface)
    }
}

impl CompositorHandler for App {
    /// The compositor showed a frame of `surface`: the bar may draw again.
    fn frame(
        &mut self,
        _conn: &Connection,
        qh: &QueueHandle<Self>,
        surface: &WlSurface,
        _time: u32,
    ) {
        if let Some(bar) = self.bar_for_surface(surface) {
            bar.frame_done(qh);
        }
    }

    // The rest are required by the trait but irrelevant to the bar: it draws at
    // scale 1 with no transform, like the C bar, and doesn't track which
    // monitor a surface is on (each bar is created for exactly one).
    fn scale_factor_changed(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        _: &WlSurface,
        _: i32,
    ) {
    }
    fn transform_changed(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        _: &WlSurface,
        _: wl_output::Transform,
    ) {
    }
    fn surface_enter(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        _: &WlSurface,
        _: &wl_output::WlOutput,
    ) {
    }
    fn surface_leave(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        _: &WlSurface,
        _: &wl_output::WlOutput,
    ) {
    }
}

impl LayerShellHandler for App {
    /// The compositor removed a bar's layer surface (e.g. its monitor is going
    /// away): drop that bar, which destroys its surface and buffers.
    fn closed(&mut self, _conn: &Connection, _qh: &QueueHandle<Self>, layer: &LayerSurface) {
        self.bars
            .retain(|bar| bar.wl_surface() != layer.wl_surface());
    }

    /// The compositor told a bar its size: allocate for it and draw.
    fn configure(
        &mut self,
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
        layer: &LayerSurface,
        configure: LayerSurfaceConfigure,
        _serial: u32,
    ) {
        let (width, height) = configure.new_size;
        if width == 0 || height == 0 {
            return; // never happens with three anchors, but would break allocation
        }
        let shm = &self.shm;
        if let Some(bar) = self
            .bars
            .iter_mut()
            .find(|bar| bar.wl_surface() == layer.wl_surface())
        {
            bar.configure(shm, width, height);
        }
    }
}

impl ShmHandler for App {
    fn shm_state(&mut self) -> &mut Shm {
        &mut self.shm
    }
}

impl ProvidesRegistryState for App {
    fn registry(&mut self) -> &mut RegistryState {
        &mut self.registry_state
    }
    // Globals that can come and go at runtime: monitors.
    registry_handlers![OutputState];
}

delegate_registry!(App);
delegate_dispatch2!(App);
