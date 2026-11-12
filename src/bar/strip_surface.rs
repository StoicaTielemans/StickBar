//! A small subsurface drawn and committed on its own, on top of a parent
//! surface (the C bar's `bar/strip_surface.c`): the media dropdown's
//! spectrum graphs, the one thing that animates continuously. Redrawing them
//! inside the dropdown meant copying and diffing the whole popup every frame;
//! a strip only ever touches its own pixels, and the parent isn't touched.
//!
//! **Desynchronized**, so its commits show without a parent commit. Its
//! *position* is parent state per the protocol, though: [`StripSurface::place`]
//! takes effect with the parent's next commit, which always follows (a strip
//! only moves when the parent's layout changes and repaints).
//!
//! **Frame pacing:** a draw while the last frame hasn't been shown yet (frame
//! callback pending) is only remembered, and done when the callback arrives.
//! So it never outpaces the compositor, and costs nothing while the
//! compositor isn't showing it. Takes no pointer input (empty input region):
//! clicks land on the parent as if it weren't there.

use smithay_client_toolkit::{
    compositor::{CompositorState, FrameCallbackData, Region},
    reexports::client::{
        QueueHandle,
        protocol::{wl_subsurface::WlSubsurface, wl_surface::WlSurface},
    },
    shm::Shm,
    subcompositor::SubcompositorState,
};

use crate::wayland::{App, shm_buffers::ShmBuffers};

/// What creating and drawing strips needs from the app.
pub(crate) struct StripEnv<'a> {
    /// The event queue new objects are attached to.
    pub(crate) qh: &'a QueueHandle<App>,
    /// Creates the input region.
    pub(crate) compositor: &'a CompositorState,
    /// Creates subsurfaces.
    pub(crate) subcompositor: &'a SubcompositorState,
    /// Allocates the buffers.
    pub(crate) shm: &'a Shm,
}

/// One strip.
pub(crate) struct StripSurface {
    /// Its own surface.
    surface: WlSurface,
    /// Its role on the parent.
    subsurface: WlSubsurface,
    /// The parent it was made on (a new parent surface needs a new strip).
    parent: WlSurface,
    /// Two buffers of `size`, or `None` before the first `place`.
    buffers: Option<ShmBuffers>,
    /// Its size in px.
    size: (i32, i32),
    /// A buffer is attached (it's mapped).
    shown: bool,
    /// The last commit's frame callback hasn't come back yet.
    frame_pending: bool,
    /// A draw arrived while waiting: the owner redraws on the callback.
    dirty: bool,
}

impl StripSurface {
    /// A new, empty, desynchronized strip on `parent`.
    pub(crate) fn new(env: &StripEnv<'_>, parent: &WlSurface) -> Self {
        let (subsurface, surface) = env.subcompositor.create_subsurface(parent.clone(), env.qh);
        subsurface.set_desync();
        // Empty input region: pointer events fall through to the parent.
        match Region::new(env.compositor) {
            Ok(region) => surface.set_input_region(Some(region.wl_region())),
            Err(err) => eprintln!("stickBar: couldn't make a strip's input region: {err}"),
        }
        Self {
            surface,
            subsurface,
            parent: parent.clone(),
            buffers: None,
            size: (0, 0),
            shown: false,
            frame_pending: false,
            dirty: false,
        }
    }

    /// Whether `surface` is this strip's own (frame callbacks are routed by it).
    pub(crate) fn is(&self, surface: &WlSurface) -> bool {
        &self.surface == surface
    }

    /// The parent it was made on.
    pub(crate) const fn parent(&self) -> &WlSurface {
        &self.parent
    }

    /// Moves it to (x, y) on the parent (applied with the parent's next
    /// commit) and sizes it `w` x `h`; a new size reallocates its buffers.
    pub(crate) fn place(&mut self, shm: &Shm, (x, y): (i32, i32), (w, h): (i32, i32)) {
        self.subsurface.set_position(x, y);
        if (w, h) != self.size || self.buffers.is_none() {
            match ShmBuffers::new(shm, w, h, w.saturating_mul(4)) {
                Ok(buffers) => {
                    self.buffers = Some(buffers);
                    self.size = (w, h);
                }
                Err(err) => {
                    eprintln!("stickBar: failed to allocate a {w}x{h} strip buffer: {err}");
                    self.buffers = None;
                    self.size = (0, 0);
                }
            }
        }
    }

    /// Paints the whole strip with `fill` (it gets the buffer's bytes:
    /// ARGB32, `w * 4` per row) and commits, mapping it if hidden. While the
    /// last frame is still pending this only marks it dirty: the owner
    /// redraws when [`Self::frame_done`] says so.
    pub(crate) fn draw(&mut self, qh: &QueueHandle<App>, fill: impl FnOnce(&mut [u8]) -> bool) {
        if self.frame_pending {
            self.dirty = true;
            return;
        }
        self.dirty = false;
        let Some(buffers) = self.buffers.as_mut() else {
            return;
        };
        // Both buffers still held (rare behind the frame callback): the next
        // draw retries.
        if !buffers.write_and_attach(&self.surface, fill) {
            return;
        }
        let (w, h) = self.size;
        self.surface.damage_buffer(0, 0, w, h);
        let _frame_callback = self
            .surface
            .frame(qh, FrameCallbackData(self.surface.clone()));
        self.surface.commit();
        self.frame_pending = true;
        self.shown = true;
    }

    /// Its frame callback arrived. Returns whether a draw is waiting (the
    /// owner then draws again).
    pub(crate) fn frame_done(&mut self) -> bool {
        self.frame_pending = false;
        std::mem::take(&mut self.dirty)
    }

    /// Unmaps it (no buffer); the next draw maps it again.
    pub(crate) fn hide(&mut self) {
        if !self.shown {
            return;
        }
        self.surface.attach(None, 0, 0);
        self.surface.commit();
        self.shown = false;
        self.frame_pending = false;
        self.dirty = false;
    }

    /// Shown and not waiting on the compositor: a draw now would really
    /// reach the screen. The visualizer skips analysis for strips that aren't.
    pub(crate) const fn ready(&self) -> bool {
        self.shown && !self.frame_pending
    }
}

impl Drop for StripSurface {
    fn drop(&mut self) {
        self.subsurface.destroy();
        self.surface.destroy();
    }
}
