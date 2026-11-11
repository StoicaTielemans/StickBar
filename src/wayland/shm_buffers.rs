//! Two shared-memory buffers for one surface (the C bar's `wayland/shm_buffer.c`),
//! shared by the bar and every popup.
//!
//! The compositor reads a buffer for as long as it's attached, and until it
//! sends `release` the client mustn't write into it. With two, the bar can
//! always draw into whichever one the compositor isn't holding. Both live in
//! one memfd-backed pool (SCTK's `SlotPool`, which owns and closes the fd).
//! Pages only count towards memory use once written, so the second buffer
//! costs nothing until it's actually needed.

use smithay_client_toolkit::{
    reexports::client::protocol::{wl_shm, wl_surface::WlSurface},
    shm::{
        Shm,
        slot::{Buffer, SlotPool},
    },
};

/// A surface's pair of buffers, both the same size, ARGB8888.
pub(crate) struct ShmBuffers {
    /// The pool both buffers live in.
    pool: SlotPool,
    /// First buffer.
    a: Buffer,
    /// Second buffer.
    b: Buffer,
}

impl ShmBuffers {
    /// Allocates both buffers. `stride` is bytes per row, normally cairo's own
    /// (`cairo::Format::ARgb32.stride_for_width`), so a cairo image copies in
    /// byte for byte. Cairo's ARGB32 is native-endian, which on x86 is exactly
    /// `wl_shm`'s ARGB8888.
    pub(crate) fn new(shm: &Shm, width: i32, height: i32, stride: i32) -> Result<Self, String> {
        let frame_bytes = usize::try_from(stride)
            .ok()
            .zip(usize::try_from(height).ok())
            .map(|(s, h)| s * h)
            .ok_or("negative stride or height")?;
        let mut pool = SlotPool::new(frame_bytes * 2, shm).map_err(|e| e.to_string())?;
        let (a, _) = pool
            .create_buffer(width, height, stride, wl_shm::Format::Argb8888)
            .map_err(|e| e.to_string())?;
        let (b, _) = pool
            .create_buffer(width, height, stride, wl_shm::Format::Argb8888)
            .map_err(|e| e.to_string())?;
        Ok(Self { pool, a, b })
    }

    /// Writes into whichever buffer the compositor isn't holding (`fill` gets
    /// its bytes) and attaches it to `surface`. The caller then damages what
    /// changed (damage only ever adds up, so damaging everything here would undo
    /// the bar's narrow damage) and commits. Returns false if both
    /// buffers are still held (the caller then tries again later) or `fill`
    /// reported failure.
    pub(crate) fn write_and_attach(
        &mut self,
        surface: &WlSurface,
        fill: impl FnOnce(&mut [u8]) -> bool,
    ) -> bool {
        // `canvas()` is `None` while the compositor holds a buffer.
        let buffer = if self.a.canvas(&mut self.pool).is_some() {
            &self.a
        } else {
            &self.b
        };
        let Some(canvas) = buffer.canvas(&mut self.pool) else {
            return false;
        };
        if !fill(canvas) {
            return false;
        }
        if let Err(err) = buffer.attach_to(surface) {
            eprintln!("stickBar: attaching a buffer failed: {err}");
            return false;
        }
        true
    }
}

/// Copies `pixels` into `canvas`, as a `write_and_attach` fill. They're the
/// same size by construction; checked anyway, because `copy_from_slice` would
/// panic on a mismatch.
pub(crate) fn copy_pixels(canvas: &mut [u8], pixels: &[u8]) -> bool {
    if canvas.len() != pixels.len() {
        eprintln!("stickBar: frame size doesn't match its shm buffer");
        return false;
    }
    canvas.copy_from_slice(pixels);
    true
}
