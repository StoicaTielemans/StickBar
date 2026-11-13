//! Finding which part of a frame changed, so a commit only damages that
//! rectangle (the C bar's `diff_bounds()` in `bar/bar_surface.c`).
//!
//! Telling the compositor "only this rectangle changed" means it re-uploads and
//! recomposites a few dozen pixels of clock digits on a typical tick instead of
//! the whole bar. And when nothing changed at all, the caller skips the commit
//! entirely, which covers every redraw trigger without each one needing its own
//! change detection.

/// A rectangle in buffer pixels.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Rect {
    /// Left edge.
    pub(crate) x: usize,
    /// Top edge.
    pub(crate) y: usize,
    /// Width.
    pub(crate) w: usize,
    /// Height.
    pub(crate) h: usize,
}

/// Bytes per pixel in the ARGB32 frames the bar draws.
const BYTES_PER_PIXEL: usize = 4;

/// The smallest rectangle containing every pixel that differs between frames
/// `a` and `b`, or `None` if they're identical.
///
/// Both frames are `width` pixels wide with rows `stride` bytes apart (cairo may
/// pad rows, so only the first `width * 4` bytes of each row are compared).
/// Frames of different lengths are reported as entirely changed, which is
/// always a safe answer.
pub(crate) fn changed_rect(a: &[u8], b: &[u8], width: usize, stride: usize) -> Option<Rect> {
    let row_bytes = width * BYTES_PER_PIXEL;
    let height = a.len().checked_div(stride).unwrap_or(0);
    if a.len() != b.len() || row_bytes > stride {
        return Some(Rect {
            x: 0,
            y: 0,
            w: width,
            h: height,
        });
    }

    // Running bounds of the changed area: min_x starts past the right edge and
    // max_x/max_y before the left/top, so the first changed pixel sets them.
    let mut min_x = width;
    let mut max_x = 0;
    let mut min_y = None;
    let mut max_y = 0;

    for (y, (row_a, row_b)) in a
        .chunks_exact(stride)
        .zip(b.chunks_exact(stride))
        .enumerate()
    {
        let (Some(row_a), Some(row_b)) = (row_a.get(..row_bytes), row_b.get(..row_bytes)) else {
            continue;
        };
        // Whole-row compare first: it's a single memcmp, and most rows of a
        // typical redraw are unchanged, so most rows stop here.
        if row_a == row_b {
            continue;
        }
        if min_y.is_none() {
            min_y = Some(y);
        }
        max_y = y;

        // Pixel pairs of this row, scanned from each end for the first difference.
        // `as_chunks` views a row as `[u8; 4]` pixels, so comparing two pixels
        // compares 4 bytes at once.
        let pixels = || {
            let (pixels_a, _) = row_a.as_chunks::<BYTES_PER_PIXEL>();
            let (pixels_b, _) = row_b.as_chunks::<BYTES_PER_PIXEL>();
            pixels_a.iter().zip(pixels_b)
        };
        if let Some(left) = pixels().position(|(pa, pb)| pa != pb) {
            min_x = min_x.min(left);
        }
        if let Some(right) = pixels().rposition(|(pa, pb)| pa != pb) {
            max_x = max_x.max(right);
        }
    }

    let min_y = min_y?;
    Some(Rect {
        x: min_x,
        y: min_y,
        w: max_x - min_x + 1,
        h: max_y - min_y + 1,
    })
}

#[cfg(test)]
mod tests {
    use super::{Rect, changed_rect};

    /// A blank frame `width` x `height` pixels, with `pad` extra bytes per row
    /// (like cairo's row padding).
    fn frame(width: usize, height: usize, pad: usize) -> Vec<u8> {
        vec![0; (width * 4 + pad) * height]
    }

    /// Sets one byte of pixel (x, y) to 0xff.
    fn poke(buf: &mut [u8], x: usize, y: usize, stride: usize) {
        buf[y * stride + x * 4] = 0xff;
    }

    #[test]
    fn identical_frames_have_no_damage() {
        let a = frame(8, 4, 0);
        assert_eq!(changed_rect(&a, &a.clone(), 8, 32), None);
    }

    #[test]
    fn one_pixel_gives_a_one_pixel_rect() {
        let a = frame(8, 4, 0);
        let mut b = a.clone();
        poke(&mut b, 3, 2, 32);
        assert_eq!(
            changed_rect(&a, &b, 8, 32),
            Some(Rect {
                x: 3,
                y: 2,
                w: 1,
                h: 1
            })
        );
    }

    #[test]
    fn spans_every_changed_pixel() {
        let a = frame(8, 4, 0);
        let mut b = a.clone();
        poke(&mut b, 0, 1, 32); // left edge
        poke(&mut b, 7, 3, 32); // right edge, bottom row
        assert_eq!(
            changed_rect(&a, &b, 8, 32),
            Some(Rect {
                x: 0,
                y: 1,
                w: 8,
                h: 3
            })
        );
    }

    #[test]
    fn row_padding_is_ignored() {
        // 8 px wide = 32 bytes per row, plus 8 bytes of padding per row.
        let a = frame(8, 2, 8);
        let mut b = a.clone();
        b[32] = 0xff; // first padding byte of row 0
        assert_eq!(changed_rect(&a, &b, 8, 40), None);
    }

    #[test]
    fn different_sizes_count_as_fully_changed() {
        let a = frame(8, 4, 0);
        let b = frame(8, 3, 0);
        assert_eq!(
            changed_rect(&a, &b, 8, 32),
            Some(Rect {
                x: 0,
                y: 0,
                w: 8,
                h: 4
            })
        );
    }
}
