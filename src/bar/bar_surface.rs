//! One monitor's bar: its layer surface, its pixel buffers, and the redraw
//! logic that commits only what changed (the C bar's `bar/bar_surface.c`).
//!
//! The redraw pipeline, the same as the C bar's:
//!
//!   1. Draw the whole bar into the private `next` frame (a cairo image).
//!   2. Compare `next` with `prev`, the frame the compositor currently shows.
//!      Identical means no commit at all. Most redraw triggers (an event that
//!      didn't change anything visible) end here.
//!   3. Otherwise copy `next` into whichever of the two shared-memory buffers
//!      the compositor isn't holding, commit it with damage limited to the
//!      changed rectangle, and swap `prev`/`next`.
//!
//! Step 1 has a shortcut for the clock, which changes far more often than
//! anything else: when only the clock is dirty, `next` starts as a copy of
//! `prev` and just the clock's box is repainted on top, so nothing else is laid
//! out again. If the clock's width changed, everything left of it would move,
//! so that falls back to a full redraw.
//!
//! Redraws are paced by frame callbacks: after a commit, the next one waits for
//! the compositor's "frame done" so the bar never draws faster than the
//! monitor shows. A redraw requested meanwhile just sets `dirty`.

use smithay_client_toolkit::{
    compositor::{CompositorState, FrameCallbackData, Region},
    reexports::client::{
        QueueHandle,
        protocol::{wl_output::WlOutput, wl_surface::WlSurface},
    },
    shell::{
        WaylandSurface,
        wlr_layer::{Anchor, KeyboardInteractivity, Layer, LayerShell, LayerSurface},
    },
    shm::Shm,
};

use super::damage::{Rect, changed_rect};
use crate::{
    config::BAR,
    render::{BarLayout, ClickTarget, ModuleBox, Renderer},
    wayland::{
        App,
        shm_buffers::{ShmBuffers, copy_pixels},
    },
};

/// The layer-shell namespace. Kept identical to the C bar's, so any Hyprland
/// `layerrule` written for it keeps matching.
const NAMESPACE: &str = "custombar";

/// How much of the bar needs redrawing. Ordered, so merging two requests is
/// `max`: a full redraw covers a clock-only one.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
enum Dirty {
    /// Nothing changed.
    Clean,
    /// Only the clock's text changed.
    Clock,
    /// Anything else (or the size) changed: redraw everything.
    Full,
}

/// Everything that depends on the surface's size, (re)created on each configure
/// that changes the size. Grouped so "not configured yet" is simply `None`.
struct Frames {
    /// Width in pixels.
    width: i32,
    /// Height in pixels.
    height: i32,
    /// Bytes per row, as cairo lays out ARGB32 images (shared with the shm buffers).
    stride: i32,
    /// The two shared-memory buffers the compositor reads from.
    buffers: ShmBuffers,
    /// The frame the compositor currently shows.
    prev: cairo::ImageSurface,
    /// The frame being drawn now; swapped with `prev` after each commit.
    next: cairo::ImageSurface,
    /// False until the first commit, so the first frame always damages everything.
    prev_valid: bool,
}

/// One monitor's bar.
pub(crate) struct BarSurface {
    /// The layer surface (anchored top/left/right, 20 px tall, reserving its space).
    layer: LayerSurface,
    /// Size-dependent state; `None` until the compositor's first configure.
    frames: Option<Frames>,
    /// What may have changed since the last frame: redrawn at the next chance.
    dirty: Dirty,
    /// Where each module's box and click target was drawn in the last frame
    /// (the C bar's per-module box-rect tables, kept per bar here). Read by
    /// the tests' `rects` command and by click hit-testing.
    layout: BarLayout,
    /// The monitor's output index, which picks its workspace range.
    output_index: usize,
    /// A frame callback is outstanding, so the next redraw waits for it.
    frame_pending: bool,
}

impl BarSurface {
    /// Creates the bar's layer surface on `output` and commits it once with no
    /// buffer, which asks the compositor for the first configure (the size).
    ///
    /// In test mode the surface gets an empty input region first, so it takes no
    /// pointer input at all: the user's real clicks pass through to whatever is
    /// underneath instead of closing a test's popups mid-test (which happened
    /// with the C bar, once while the user was in a fullscreen game). Tests click
    /// through the test-control channel instead.
    pub(crate) fn new(
        qh: &QueueHandle<App>,
        compositor: &CompositorState,
        layer_shell: &LayerShell,
        output: &WlOutput,
        output_index: usize,
        test_mode: bool,
    ) -> Self {
        let surface = compositor.create_surface(qh);
        if test_mode {
            // A region with nothing added to it is empty. The surface keeps its
            // own copy of the region, so this one can be dropped right after.
            match Region::new(compositor) {
                Ok(region) => surface.set_input_region(Some(region.wl_region())),
                Err(err) => eprintln!("stickBar: couldn't create an empty input region: {err}"),
            }
        }
        let layer = layer_shell.create_layer_surface(
            qh,
            surface,
            Layer::Top,
            Some(NAMESPACE),
            Some(output),
        );
        // Width 0 = "as wide as the anchors allow", i.e. the full monitor width.
        layer.set_size(0, u32::from(BAR.height));
        layer.set_anchor(Anchor::TOP | Anchor::LEFT | Anchor::RIGHT);
        layer.set_exclusive_zone(i32::from(BAR.height));
        layer.set_keyboard_interactivity(KeyboardInteractivity::None);
        layer.commit();
        Self {
            layer,
            frames: None,
            dirty: Dirty::Clean,
            layout: BarLayout::default(),
            output_index,
            frame_pending: false,
        }
    }

    /// Asks for a full redraw at the next opportunity.
    pub(crate) fn mark_dirty(&mut self) {
        self.dirty = self.dirty.max(Dirty::Full);
    }

    /// Asks for the clock to be redrawn (only) at the next opportunity.
    pub(crate) fn mark_clock_dirty(&mut self) {
        self.dirty = self.dirty.max(Dirty::Clock);
    }

    /// Where each module's box was drawn in the last frame.
    pub(crate) fn boxes(&self) -> &[ModuleBox] {
        &self.layout.boxes
    }

    /// Where each click target was drawn in the last frame.
    pub(crate) fn targets(&self) -> &[ClickTarget] {
        &self.layout.targets
    }

    /// The bar's `wl_surface`, for matching frame callbacks and pointer events.
    pub(crate) fn wl_surface(&self) -> &WlSurface {
        self.layer.wl_surface()
    }

    /// Handles a configure: (re)allocates the buffers if the size changed, then
    /// marks the bar dirty so the next flush draws it. SCTK has already acked
    /// the configure by the time this runs.
    pub(crate) fn configure(&mut self, shm: &Shm, width: u32, height: u32) {
        let unchanged = self.frames.as_ref().is_some_and(|f| {
            u32::try_from(f.width).ok() == Some(width)
                && u32::try_from(f.height).ok() == Some(height)
        });
        if !unchanged {
            // Drop the old buffers first, so their memory is freed before the new
            // pool is allocated rather than after.
            self.frames = None;
            match Frames::new(shm, width, height) {
                Ok(frames) => self.frames = Some(frames),
                Err(err) => {
                    eprintln!("stickBar: failed to allocate a {width}x{height} bar surface: {err}");
                    return;
                }
            }
        }
        self.dirty = Dirty::Full;
    }

    /// Redraws now if the bar is dirty, configured, and not waiting on a frame
    /// callback. Called after every pass of the event loop.
    pub(crate) fn flush(&mut self, qh: &QueueHandle<App>, renderer: &mut Renderer) {
        if self.dirty != Dirty::Clean && !self.frame_pending && self.frames.is_some() {
            self.render(qh, renderer);
        }
    }

    /// The compositor showed the last frame: draw again if anything changed meanwhile.
    pub(crate) fn frame_done(&mut self, qh: &QueueHandle<App>, renderer: &mut Renderer) {
        self.frame_pending = false;
        self.flush(qh, renderer);
    }

    /// Steps 1-3 of the pipeline in the module docs.
    fn render(&mut self, qh: &QueueHandle<App>, renderer: &mut Renderer) {
        let Some(frames) = self.frames.as_mut() else {
            return;
        };

        // 1. Draw into `next` (all of it, or just the clock).
        if let Err(err) = frames.draw(renderer, &mut self.layout, self.dirty, self.output_index) {
            eprintln!("stickBar: drawing the bar failed: {err}");
            return;
        }

        // 2. What changed since the frame on screen?
        let Some(damage) = frames.damage() else {
            self.dirty = Dirty::Clean; // nothing visible changed: no commit at all
            return;
        };

        // 3. Copy into a free shm buffer. If the compositor still holds both,
        // stay dirty: the release event wakes the loop, and the flush after
        // that pass draws again.
        let surface = self.layer.wl_surface();
        let mut read_ok = false;
        let next = &frames.next;
        let attached = frames.buffers.write_and_attach(surface, |canvas| {
            let mut copied = false;
            read_ok = next
                .with_data(|pixels| copied = copy_pixels(canvas, pixels))
                .is_ok();
            read_ok && copied
        });
        if !attached {
            if !read_ok {
                eprintln!("stickBar: couldn't read the drawn bar frame");
            }
            return;
        }
        if let Some((x, y, w, h)) = damage.to_i32() {
            surface.damage_buffer(x, y, w, h);
        } else {
            surface.damage_buffer(0, 0, frames.width, frames.height);
        }
        // The callback object itself isn't needed: its "done" event arrives
        // through CompositorHandler::frame with the surface attached.
        let _frame_callback = surface.frame(qh, FrameCallbackData(surface.clone()));
        self.layer.commit();

        std::mem::swap(&mut frames.prev, &mut frames.next);
        frames.prev_valid = true;
        self.frame_pending = true;
        self.dirty = Dirty::Clean;
    }
}

impl Frames {
    /// Allocates the pool, both shm buffers and both cairo frames for a
    /// `width` x `height` surface.
    fn new(shm: &Shm, width: u32, height: u32) -> Result<Self, String> {
        let width = i32::try_from(width).map_err(|e| e.to_string())?;
        let height = i32::try_from(height).map_err(|e| e.to_string())?;
        let prev = cairo::ImageSurface::create(cairo::Format::ARgb32, width, height)
            .map_err(|e| e.to_string())?;
        let next = cairo::ImageSurface::create(cairo::Format::ARgb32, width, height)
            .map_err(|e| e.to_string())?;
        // Use cairo's own stride for the shm buffers too, so a frame copies
        // into a buffer byte for byte. Cairo's ARGB32 is native-endian, which
        // on x86 is exactly wl_shm's ARGB8888.
        let stride = next.stride();
        let buffers = ShmBuffers::new(shm, width, height, stride)?;
        Ok(Self {
            width,
            height,
            stride,
            buffers,
            prev,
            next,
            prev_valid: false,
        })
    }

    /// Step 1: draws the bar into `next` and records where the boxes went.
    /// Every cairo context made here is dropped before returning, which the
    /// pixel reads after it need (cairo won't share a surface's pixels while a
    /// context can still draw on it).
    fn draw(
        &mut self,
        renderer: &mut Renderer,
        layout: &mut BarLayout,
        dirty: Dirty,
        output_index: usize,
    ) -> Result<(), cairo::Error> {
        let (width, height) = (f64::from(self.width), f64::from(self.height));
        let old_clock = layout
            .boxes
            .iter()
            .find(|b| b.name == "clock")
            .map(|b| b.rect);

        // The clock-only shortcut needs the previous picture and the clock's old box.
        if dirty == Dirty::Clock
            && self.prev_valid
            && let Some(old) = old_clock
            && self.copy_prev_into_next()
        {
            let cr = cairo::Context::new(&self.next)?;
            if renderer.render_clock_only(&cr, old)?.is_some() {
                drop(cr);
                self.next.flush();
                return Ok(()); // same box, so `layout` is still right
            }
            // The clock's width changed: fall through to a full redraw, which
            // paints over everything anyway.
        }

        let cr = cairo::Context::new(&self.next)?;
        *layout = renderer.render_output(&cr, width, height, output_index)?;
        drop(cr);
        self.next.flush();
        Ok(())
    }

    /// Copies the frame on screen into `next`, as the starting point for a
    /// clock-only redraw. Returns false if the pixels couldn't be copied.
    fn copy_prev_into_next(&mut self) -> bool {
        // `data()` hands out the pixels for writing (and tells cairo they
        // changed when it's dropped); `with_data()` reads the other frame's.
        let Ok(mut next) = self.next.data() else {
            return false;
        };
        let mut copied = false;
        let read = self.prev.with_data(|prev| {
            // Same size by construction; checked because copy_from_slice
            // would panic on a mismatch.
            if next.len() == prev.len() {
                next.copy_from_slice(prev);
                copied = true;
            }
        });
        read.is_ok() && copied
    }

    /// The changed rectangle between `prev` and `next`, or `None` if nothing
    /// changed. Before the first commit everything counts as changed.
    fn damage(&self) -> Option<Rect> {
        let full = usize::try_from(self.width)
            .ok()
            .zip(usize::try_from(self.height).ok())
            .map(|(w, h)| Rect { x: 0, y: 0, w, h });
        if !self.prev_valid {
            return full;
        }
        let (Ok(width), Ok(stride)) = (usize::try_from(self.width), usize::try_from(self.stride))
        else {
            return full;
        };
        let mut result = full;
        let mut read_next = false;
        let read_prev = self.prev.with_data(|prev| {
            read_next = self
                .next
                .with_data(|next| result = changed_rect(prev, next, width, stride))
                .is_ok();
        });
        // If either frame couldn't be read, damaging everything is always safe.
        if read_prev.is_ok() && read_next {
            result
        } else {
            full
        }
    }
}

impl Rect {
    /// The rectangle as the `i32`s `wl_surface.damage_buffer` takes, or `None`
    /// if it doesn't fit (it always does for a real monitor).
    fn to_i32(self) -> Option<(i32, i32, i32, i32)> {
        Some((
            i32::try_from(self.x).ok()?,
            i32::try_from(self.y).ok()?,
            i32::try_from(self.w).ok()?,
            i32::try_from(self.h).ok()?,
        ))
    }
}
