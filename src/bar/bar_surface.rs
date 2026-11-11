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
//! Redraws are paced by frame callbacks: after a commit, the next one waits for
//! the compositor's "frame done" so the bar never draws faster than the
//! monitor shows. A redraw requested meanwhile just sets `dirty`.

use smithay_client_toolkit::{
    compositor::{CompositorState, FrameCallbackData},
    reexports::client::{
        QueueHandle,
        protocol::{wl_output::WlOutput, wl_shm, wl_surface::WlSurface},
    },
    shell::{
        WaylandSurface,
        wlr_layer::{Anchor, KeyboardInteractivity, Layer, LayerShell, LayerSurface},
    },
    shm::{
        Shm,
        slot::{Buffer, SlotPool},
    },
};

use super::damage::{Rect, changed_rect};
use crate::{config::BAR, render, wayland::App};

/// The layer-shell namespace. Kept identical to the C bar's, so any Hyprland
/// `layerrule` written for it keeps matching.
const NAMESPACE: &str = "custombar";

/// Everything that depends on the surface's size, (re)created on each configure
/// that changes the size. Grouped so "not configured yet" is simply `None`.
struct Frames {
    /// Width in pixels.
    width: i32,
    /// Height in pixels.
    height: i32,
    /// Bytes per row, as cairo lays out ARGB32 images (shared with the shm buffers).
    stride: i32,
    /// The shared-memory pool both buffers live in (the memfd the compositor reads).
    pool: SlotPool,
    /// First of the two shm buffers: while the compositor holds one, the bar can
    /// draw into the other.
    buffer_a: Buffer,
    /// Second of the two shm buffers.
    buffer_b: Buffer,
    /// The frame the compositor currently shows.
    prev: cairo::ImageSurface,
    /// The frame being drawn now; swapped with `prev` after each commit.
    next: cairo::ImageSurface,
    /// False until the first commit, so the first frame always damages everything.
    prev_valid: bool,
}

/// One monitor's bar.
pub(crate) struct BarSurface {
    /// The monitor this bar is on.
    output: WlOutput,
    /// The layer surface (anchored top/left/right, 20 px tall, reserving its space).
    layer: LayerSurface,
    /// Size-dependent state; `None` until the compositor's first configure.
    frames: Option<Frames>,
    /// Something shown on this bar may have changed: redraw at the next chance.
    dirty: bool,
    /// A frame callback is outstanding, so the next redraw waits for it.
    frame_pending: bool,
}

impl BarSurface {
    /// Creates the bar's layer surface on `output` and commits it once with no
    /// buffer, which asks the compositor for the first configure (the size).
    pub(crate) fn new(
        qh: &QueueHandle<App>,
        compositor: &CompositorState,
        layer_shell: &LayerShell,
        output: WlOutput,
    ) -> Self {
        let surface = compositor.create_surface(qh);
        let layer = layer_shell.create_layer_surface(
            qh,
            surface,
            Layer::Top,
            Some(NAMESPACE),
            Some(&output),
        );
        // Width 0 = "as wide as the anchors allow", i.e. the full monitor width.
        layer.set_size(0, u32::from(BAR.height));
        layer.set_anchor(Anchor::TOP | Anchor::LEFT | Anchor::RIGHT);
        layer.set_exclusive_zone(i32::from(BAR.height));
        layer.set_keyboard_interactivity(KeyboardInteractivity::None);
        layer.commit();
        Self {
            output,
            layer,
            frames: None,
            dirty: false,
            frame_pending: false,
        }
    }

    /// The monitor this bar is on.
    pub(crate) const fn output(&self) -> &WlOutput {
        &self.output
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
        self.dirty = true;
    }

    /// Redraws now if the bar is dirty, configured, and not waiting on a frame
    /// callback. Called after every pass of the event loop.
    pub(crate) fn flush(&mut self, qh: &QueueHandle<App>) {
        if self.dirty && !self.frame_pending && self.frames.is_some() {
            self.render(qh);
        }
    }

    /// The compositor showed the last frame: draw again if anything changed meanwhile.
    pub(crate) fn frame_done(&mut self, qh: &QueueHandle<App>) {
        self.frame_pending = false;
        self.flush(qh);
    }

    /// Steps 1-3 of the pipeline in the module docs.
    fn render(&mut self, qh: &QueueHandle<App>) {
        let Some(frames) = self.frames.as_mut() else {
            return;
        };

        // 1. Draw into `next`. The cairo context must be gone before the pixels
        // are read, so it lives only inside this block.
        {
            let cr = match cairo::Context::new(&frames.next) {
                Ok(cr) => cr,
                Err(err) => {
                    eprintln!("stickBar: cairo context failed: {err}");
                    return;
                }
            };
            if let Err(err) = render::render_output(&cr) {
                eprintln!("stickBar: drawing the bar failed: {err}");
                return;
            }
        }
        frames.next.flush();

        // 2. What changed since the frame on screen?
        let Some(damage) = frames.damage() else {
            self.dirty = false; // nothing visible changed: no commit at all
            return;
        };

        // 3. Copy into a free shm buffer. If the compositor still holds both,
        // stay dirty: the release event wakes the loop, and the flush after
        // that pass draws again.
        let buffer = if frames.buffer_a.canvas(&mut frames.pool).is_some() {
            &frames.buffer_a
        } else {
            &frames.buffer_b
        };
        let Some(canvas) = buffer.canvas(&mut frames.pool) else {
            return;
        };
        let mut copied = false;
        let read = frames.next.with_data(|pixels| {
            // Same size by construction; checked anyway because copy_from_slice
            // would panic on a mismatch.
            if canvas.len() == pixels.len() {
                canvas.copy_from_slice(pixels);
                copied = true;
            }
        });
        if read.is_err() || !copied {
            eprintln!("stickBar: couldn't copy the frame into the shm buffer");
            return;
        }

        let surface = self.layer.wl_surface();
        if let Err(err) = buffer.attach_to(surface) {
            eprintln!("stickBar: attaching the bar buffer failed: {err}");
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
        self.dirty = false;
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
        let frame_bytes = usize::try_from(stride)
            .ok()
            .zip(usize::try_from(height).ok())
            .map(|(s, h)| s * h)
            .ok_or("negative stride or height")?;
        let mut pool = SlotPool::new(frame_bytes * 2, shm).map_err(|e| e.to_string())?;
        let (buffer_a, _) = pool
            .create_buffer(width, height, stride, wl_shm::Format::Argb8888)
            .map_err(|e| e.to_string())?;
        let (buffer_b, _) = pool
            .create_buffer(width, height, stride, wl_shm::Format::Argb8888)
            .map_err(|e| e.to_string())?;
        Ok(Self {
            width,
            height,
            stride,
            pool,
            buffer_a,
            buffer_b,
            prev,
            next,
            prev_valid: false,
        })
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
