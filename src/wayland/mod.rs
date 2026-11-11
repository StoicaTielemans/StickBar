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
mod pointer;
pub(crate) mod shm_buffers;
pub(crate) mod viewporter;

pub(crate) use output::Output;
pub(crate) use pointer::{BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, Mouse};

use smithay_client_toolkit::{
    compositor::{CompositorHandler, CompositorState},
    delegate_dispatch2, delegate_registry,
    output::OutputState,
    reexports::client::{
        Connection, QueueHandle,
        globals::GlobalList,
        protocol::{wl_output, wl_surface::WlSurface},
    },
    reexports::protocols::wp::viewporter::client::wp_viewporter::WpViewporter,
    registry::{ProvidesRegistryState, RegistryState},
    registry_handlers,
    seat::{SeatState, pointer::cursor_shape::CursorShapeManager},
    shell::{
        WaylandSurface,
        wlr_layer::{LayerShell, LayerShellHandler, LayerSurface, LayerSurfaceConfigure},
    },
    shm::{Shm, ShmHandler},
};

use crate::{
    bar::{BarSurface, popups::Popups},
    hyprland::dispatch::Dispatcher,
    render::Renderer,
};

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
    /// `wp_viewporter`, if offered: lets click-catchers use a 1x1 buffer.
    viewporter: Option<WpViewporter>,
    /// Tracks seats (input devices) and reports them to `pointer.rs`.
    seat_state: SeatState,
    /// The mouse, once the seat reports one.
    pointer: Option<Mouse>,
    /// `wp_cursor_shape_manager_v1`, if offered: sets the hand cursor.
    cursor_shapes: Option<CursorShapeManager>,
    /// The handle new Wayland objects are created with (popups need it when a
    /// click opens one).
    qh: QueueHandle<Self>,
    /// Every dropdown (open or closed).
    popups: Popups,
    /// Every tracked monitor, each with its bar (the C bar's `Output` list).
    outputs: Vec<Output>,
    /// Drawing state shared by every bar: the text cache and the modules.
    renderer: Renderer,
    /// How many monitors have been reported so far; the next one's `index`.
    next_output_index: usize,
    /// `STICKBAR_TEST_CONTROL=1`: the tests drive this instance (see `testing/`),
    /// so its surfaces take no pointer input and the user's real clicks pass through.
    test_mode: bool,
    /// Sends Hyprland dispatchers (workspace clicks), remembering the syntax.
    dispatcher: Dispatcher,
    /// Set when Hyprland's event socket hangs up; `main.rs` decides whether
    /// to reconnect (once) and clears it.
    pub(crate) hyprland_events_lost: bool,
    /// Set by SIGINT/SIGTERM: the main loop stops after the current pass.
    pub(crate) exit: bool,
}

impl App {
    /// Binds the globals the bar can't work without. The error is the message
    /// to print, the same one the C bar prints, since the most likely cause is
    /// running under a compositor that isn't wlroots-based.
    pub(crate) fn new(
        globals: &GlobalList,
        qh: &QueueHandle<Self>,
        test_mode: bool,
    ) -> Result<Self, String> {
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
        let renderer = Renderer::new().map_err(|err| format!("couldn't set up drawing: {err}"))?;
        Ok(Self {
            registry_state: RegistryState::new(globals),
            compositor,
            shm,
            output_state: OutputState::new(globals, qh),
            layer_shell,
            viewporter: viewporter::bind(globals, qh),
            seat_state: SeatState::new(globals, qh),
            pointer: None,
            cursor_shapes: CursorShapeManager::bind(globals, qh).ok(),
            qh: qh.clone(),
            popups: Popups::default(),
            outputs: Vec::new(),
            renderer,
            next_output_index: 0,
            test_mode,
            dispatcher: Dispatcher::new(),
            hyprland_events_lost: false,
            exit: false,
        })
    }

    /// Redraws every bar that's dirty and free to draw. The main loop calls this
    /// after each pass, so everything changed in one pass is drawn once.
    pub(crate) fn flush_dirty(&mut self, qh: &QueueHandle<Self>) {
        // Destructured so the bars (in `outputs`) and the shared `renderer`
        // can be borrowed at the same time: they're separate fields.
        let Self {
            outputs, renderer, ..
        } = self;
        for bar in outputs.iter_mut().filter_map(|output| output.bar.as_mut()) {
            bar.flush(qh, renderer);
        }
    }

    /// The clock timer fired: re-read the time, and if the shown text changed,
    /// have every bar repaint just its clock.
    pub(crate) fn clock_tick(&mut self) {
        if self.renderer.clock.tick() {
            for bar in self.bars_mut() {
                bar.mark_clock_dirty();
            }
        }
    }

    /// Re-reads the workspaces from Hyprland; every bar redraws only if what
    /// the pills show changed.
    pub(crate) fn refresh_workspaces(&mut self) {
        if self.renderer.workspaces.refresh() {
            self.mark_all_dirty();
        }
    }

    /// Asks every bar for a redraw (the C bar's `output_mark_all_dirty`).
    pub(crate) fn mark_all_dirty(&mut self) {
        for bar in self.bars_mut() {
            bar.mark_dirty();
        }
    }

    /// Every tracked monitor, in the order they were reported.
    pub(crate) fn outputs(&self) -> &[Output] {
        &self.outputs
    }

    /// A monitor's current mode size in pixels, or (0, 0) if it isn't known.
    /// Read from SCTK when asked rather than stored, so it's never stale.
    pub(crate) fn output_size(&self, output: &Output) -> (i32, i32) {
        self.output_state
            .info(output.wl_output())
            .and_then(|info| info.modes.into_iter().find(|mode| mode.current))
            .map_or((0, 0), |mode| mode.dimensions)
    }

    /// Every dropdown, for the tests' `menu` and `catcher-click` commands.
    pub(crate) const fn popups(&self) -> &Popups {
        &self.popups
    }

    /// Saves the open dropdown's picture as a PNG (the tests' `snapshot`).
    pub(crate) fn snapshot(&mut self, path: &str) -> Result<(), String> {
        self.popups.snapshot(path, &mut self.renderer.text)
    }

    /// Every existing bar.
    fn bars_mut(&mut self) -> impl Iterator<Item = &mut BarSurface> {
        self.outputs
            .iter_mut()
            .filter_map(|output| output.bar.as_mut())
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
        let Self {
            outputs, renderer, ..
        } = self;
        let bar = outputs
            .iter_mut()
            .filter_map(|output| output.bar.as_mut())
            .find(|bar| bar.wl_surface() == surface);
        if let Some(bar) = bar {
            bar.frame_done(qh, renderer);
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
    /// The compositor removed a bar's layer surface: drop that bar, which
    /// destroys its surface and buffers. The monitor's record stays (it
    /// reports `has_bar: false` to the tests, like the C bar).
    fn closed(&mut self, _conn: &Connection, _qh: &QueueHandle<Self>, layer: &LayerSurface) {
        for output in &mut self.outputs {
            if output
                .bar
                .as_ref()
                .is_some_and(|bar| bar.wl_surface() == layer.wl_surface())
            {
                output.bar = None;
                return;
            }
        }
        self.popups.closed(layer.wl_surface());
    }

    /// The compositor told a surface its size: a bar allocates for it and
    /// draws; a popup surface does what `PopupSurface::configure` says.
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
        let bar = self
            .outputs
            .iter_mut()
            .filter_map(|output| output.bar.as_mut())
            .find(|bar| bar.wl_surface() == layer.wl_surface());
        if let Some(bar) = bar {
            bar.configure(shm, width, height);
            return;
        }
        // Not a bar: one of the open dropdowns' surfaces.
        let _owned: bool = self.popups.configure(
            layer.wl_surface(),
            &self.shm,
            (width, height),
            &mut self.renderer.text,
        );
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
    // Globals that can come and go at runtime: monitors and seats.
    registry_handlers![OutputState, SeatState];
}

delegate_registry!(App);
delegate_dispatch2!(App);
