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
    subcompositor::SubcompositorState,
};

use crate::{
    audio::{Audio, AudioState},
    bar::{BarSurface, popup_surface::PopupEnv, popups::Popups, strip_surface::StripEnv},
    dbus::{Command, Dbus, TrayEvent},
    hyprland::dispatch::Dispatcher,
    mpris::{MediaCommand, MediaEvent},
    pipewire::{Category, PipewireEvent, PrivacyState},
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
    /// `wl_subcompositor`, if offered: the media graphs' strips (without it
    /// the dropdown has no graph rows).
    subcompositor: Option<SubcompositorState>,
    /// The event loop, for timers started later (the visualizer's).
    pub(crate) loop_handle: Option<calloop::LoopHandle<'static, Self>>,
    /// The visualizer's 15 fps timer, while it runs.
    visualizer_timer: Option<calloop::RegistrationToken>,
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
    /// Sends volume/mute commands to the pulse thread (set in `start_sources`).
    pub(crate) audio: Audio,
    /// Sends tray commands to the `dbus` thread (set in `start_sources`).
    pub(crate) dbus: Dbus,
    /// The tray menu asked for and not answered yet (it opens on the answer).
    tray_pending: Option<pointer::PendingMenu>,
    /// The last tray menu request's number.
    tray_requests: u64,
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
        let mut renderer =
            Renderer::new().map_err(|err| format!("couldn't set up drawing: {err}"))?;
        let subcompositor =
            SubcompositorState::bind(compositor.wl_compositor().clone(), globals, qh).ok();
        renderer.media.visualizer = crate::visualizer::Visualizer::new(subcompositor.is_some());
        Ok(Self {
            registry_state: RegistryState::new(globals),
            compositor,
            shm,
            output_state: OutputState::new(globals, qh),
            layer_shell,
            viewporter: viewporter::bind(globals, qh),
            subcompositor,
            loop_handle: None,
            visualizer_timer: None,
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
            audio: Audio::default(),
            dbus: Dbus::default(),
            tray_pending: None,
            tray_requests: 0,
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
        // The CPU dropdown's first process figures ride on this 1 s tick.
        let renderer = &mut self.renderer;
        self.popups
            .cpu
            .tick(&mut renderer.sysmon, &mut renderer.text);
        // Players don't push their position: while the media dropdown shows
        // it, it's read once a second (the answer repaints).
        if self.popups.media.is_open() {
            self.dbus.send(Command::Media(MediaCommand::PollPositions));
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
        self.popups.snapshot(path, &mut self.renderer)
    }

    /// Where a device card's control is (the tests' `target device`).
    pub(crate) fn device_target(&mut self, what: (&str, &str, f64)) -> Option<(f64, f64)> {
        self.popups
            .device
            .target(what, &self.renderer.audio, &mut self.renderer.text)
    }

    /// Where a mixer card's control is (the tests' `target mixer`).
    pub(crate) fn mixer_target(&self, what: (&str, &str, f64)) -> Option<(f64, f64)> {
        self.popups.mixer.target(what, &self.renderer.audio)
    }

    /// A new audio snapshot from the pulse thread. The bars are redrawn only
    /// if what they show changed (the C bar's `pulse_client_pump` rule).
    pub(crate) fn audio_update(&mut self, state: AudioState) {
        let changed = state.shown() != self.renderer.audio.shown();
        self.renderer.audio = state;
        if changed {
            self.mark_all_dirty();
        }
        // The open dropdowns follow every change, shown on the bar or not.
        let Self {
            qh,
            compositor,
            layer_shell,
            viewporter,
            test_mode,
            popups,
            renderer,
            ..
        } = self;
        let env = PopupEnv {
            qh,
            compositor,
            layer_shell,
            viewporter: viewporter.as_ref(),
            test_mode: *test_mode,
        };
        popups
            .device
            .refresh(&env, &renderer.audio, &mut renderer.text);
        popups
            .mixer
            .refresh(&env, &renderer.audio, &mut renderer.text);
    }

    /// The 5 s poll (the C bar's 5 s branch, in its order): sysmon figures and
    /// one history sample, the open sysmon dropdown refreshed, then the
    /// network the same way, and every bar redrawn (bars whose pixels didn't
    /// change skip the commit anyway).
    pub(crate) fn poll_5s(&mut self) {
        self.renderer.sysmon.poll();
        let Renderer {
            sysmon,
            text,
            network,
            ..
        } = &mut self.renderer;
        self.popups.mem.refresh_if_open(sysmon, text);
        self.popups.cpu.refresh_if_open(sysmon, text);
        self.popups.gpu.refresh_if_open(sysmon, text);
        network.poll();
        self.popups.network.refresh_if_open(network, text);
        self.mark_all_dirty();
    }

    /// News from the `dbus` thread: a menu answer opens the menu waiting for
    /// it; an item change redraws the bars (and the open menu, whose header
    /// shows the item's icon).
    pub(crate) fn tray_event(&mut self, event: TrayEvent) {
        if let TrayEvent::Menu { request, entries } = event {
            self.open_tray_menu(request, entries);
            return;
        }
        let removed = if let TrayEvent::Removed { id } = &event {
            Some(*id)
        } else {
            None
        };
        if !self.renderer.tray.apply(event) {
            return;
        }
        self.mark_all_dirty();
        if removed.is_some() && self.popups.tray.item() == removed {
            // Its app is gone: the menu's entries can't do anything any more.
            self.popups.tray.close();
        } else {
            let renderer = &mut self.renderer;
            self.popups.tray.paint(&renderer.tray, &mut renderer.text);
        }
    }

    /// News from the `dbus` thread about the media players. A new player list
    /// means something shown changed (the thread only sends it then): the
    /// bars redraw and an open dropdown re-lays out. Polled positions only
    /// repaint the dropdown.
    pub(crate) fn media_event(&mut self, event: MediaEvent) {
        match event {
            MediaEvent::Players(players) => {
                self.renderer.media.set_players(players);
                self.mark_all_dirty();
                let Self {
                    qh,
                    compositor,
                    layer_shell,
                    viewporter,
                    subcompositor,
                    shm,
                    test_mode,
                    popups,
                    renderer,
                    ..
                } = self;
                let env = PopupEnv {
                    qh,
                    compositor,
                    layer_shell,
                    viewporter: viewporter.as_ref(),
                    test_mode: *test_mode,
                };
                let strips = subcompositor.as_ref().map(|subcompositor| StripEnv {
                    qh,
                    compositor,
                    subcompositor,
                    shm,
                });
                popups
                    .media
                    .refresh((&env, strips.as_ref()), &renderer.media, &mut renderer.text);
            }
            MediaEvent::Positions(positions) => {
                for (id, position_us) in positions {
                    self.renderer.media.set_position(id, position_us);
                }
                let renderer = &mut self.renderer;
                self.popups.media.paint(&renderer.media, &mut renderer.text);
            }
        }
    }

    /// Turns the visualizer's slots on and off to match the media dropdown
    /// (open, the card's graph shown, its player playing). Called once per
    /// loop pass, like C's `media_popup_sync_visualizer`: cheap, and only a
    /// real change does anything.
    pub(crate) fn sync_visualizer(&mut self) {
        let wants = self.popups.media.slot_wants(&self.renderer.media);
        let mut start = false;
        for (slot, (active, pid)) in wants.into_iter().enumerate() {
            start |= self.renderer.media.visualizer.set_source(slot, active, pid);
        }
        if start {
            self.start_visualizer_timer();
        }
    }

    /// Starts the 15 fps animation timer, if it isn't running. It stops
    /// itself once nothing is capturing and every level has settled, so a
    /// closed dropdown costs nothing.
    fn start_visualizer_timer(&mut self) {
        if self.visualizer_timer.is_some() {
            return;
        }
        let Some(handle) = self.loop_handle.as_ref() else {
            return;
        };
        let frame = std::time::Duration::from_secs(1) / crate::visualizer::FPS;
        let inserted = handle.insert_source(
            calloop::timer::Timer::from_duration(frame),
            move |_deadline, (), app| {
                if app.visualizer_frame() {
                    calloop::timer::TimeoutAction::ToDuration(frame)
                } else {
                    calloop::timer::TimeoutAction::Drop
                }
            },
        );
        match inserted {
            Ok(token) => self.visualizer_timer = Some(token),
            Err(err) => eprintln!(
                "stickBar: couldn't start the visualizer timer: {}",
                err.error
            ),
        }
    }

    /// One visualizer frame: advance the levels, redraw the graphs whose dots
    /// moved. Returns whether the timer keeps running.
    fn visualizer_frame(&mut self) -> bool {
        let (shown, ready) = self.popups.media.graph_masks();
        let (changed, busy) = self.renderer.media.visualizer.pump(ready, shown);
        if changed != 0 {
            self.popups
                .media
                .draw_graphs(changed, &self.qh, &self.renderer.media);
        }
        if !busy {
            // The timer drops itself (TimeoutAction::Drop) on this return.
            self.visualizer_timer = None;
        }
        busy
    }

    /// News from the `pipewire` thread.
    pub(crate) fn pipewire_event(&mut self, event: PipewireEvent) {
        match event {
            PipewireEvent::Privacy(state) => self.privacy_update(state),
            PipewireEvent::Target { slot, target } => {
                self.renderer.media.visualizer.set_target(slot, target);
            }
            PipewireEvent::Unavailable => self.renderer.media.visualizer.set_unavailable(),
        }
    }

    /// Who's recording what changed (the `pipewire` thread only sends real
    /// changes). The bars redraw only if one of the three kinds turned on or
    /// off: a second app joining the mic changes the dropdown's list, not
    /// the box. An open dropdown keeps the snapshot it opened with.
    fn privacy_update(&mut self, state: PrivacyState) {
        let kinds = |s: &PrivacyState| {
            [Category::Mic, Category::Webcam, Category::ScreenShare].map(|c| s.has(c))
        };
        let changed = kinds(&state) != kinds(&self.renderer.privacy);
        self.renderer.privacy = state;
        if changed {
            self.mark_all_dirty();
        }
    }

    /// Something was written in the EQ's state directory: re-read the state
    /// file, and redraw only if it parsed.
    pub(crate) fn eq_file_changed(&mut self) {
        if self.renderer.eq.load() {
            self.eq_state_changed();
        }
    }

    /// A profile script finished (successfully or not): "applying" ends.
    pub(crate) fn eq_script_exited(&mut self) {
        self.renderer.eq.script_exited();
        self.eq_state_changed();
    }

    /// What the EQ shows changed: repaint its dropdown, if open, and the bars
    /// (bars whose pixels didn't change skip the commit anyway).
    pub(crate) fn eq_state_changed(&mut self) {
        let renderer = &mut self.renderer;
        self.popups.eq.paint(&renderer.eq, &mut renderer.text);
        self.mark_all_dirty();
    }

    /// The renderer, read-only (the tests' `target` commands).
    pub(crate) const fn renderer(&self) -> &Renderer {
        &self.renderer
    }

    /// The renderer, for the tests' `dump`/`refresh` commands.
    pub(crate) const fn renderer_mut(&mut self) -> &mut Renderer {
        &mut self.renderer
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
            return;
        }
        // Otherwise it may be one of the media graphs' strips.
        let _was_strip = self.popups.media.strip_frame_done(surface, qh);
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
            &mut self.renderer,
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
