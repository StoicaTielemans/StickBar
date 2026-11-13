//! A tray item's `IconPixmap`, turned into bytes cairo can use (the byte half
//! of the C bar's `dbus/sni_icon.c`). Pure byte work, so it runs on the `dbus`
//! thread; the main thread only wraps the result in a cairo surface and scales
//! it (`modules/tray/mod.rs`), since cairo surfaces can't cross threads.

use crate::config::{BAR, COLORS};

/// Pixels at or under this alpha are ignored when deciding whether a pixmap is
/// a flat-coloured mask: low-alpha edge pixels are noisy.
const SYMBOLIC_ALPHA_THRESHOLD: u8 = 40;
/// How tightly every sampled pixel's RGB must cluster to count as one flat colour.
const SYMBOLIC_COLOR_TOLERANCE: u8 = 12;
/// Only *dim* flat colours are recoloured: a bright one is already legible on
/// the dark bar.
const SYMBOLIC_LUMINANCE_MAX: f64 = 0.40;

/// One `IconPixmap` entry as it comes off the wire: (width, height, ARGB32
/// bytes in network byte order, i.e. `[A, R, G, B]` per pixel).
pub(crate) type Pixmap = (i32, i32, Vec<u8>);

/// A converted pixmap: premultiplied ARGB32 in the machine's own byte order,
/// exactly what a cairo `ARgb32` image surface stores, `width * 4` bytes a row.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Argb {
    /// Width in px.
    pub(crate) width: i32,
    /// Height in px.
    pub(crate) height: i32,
    /// The pixels.
    pub(crate) data: Vec<u8>,
}

/// Picks the entry whose width is closest to the bar's height (ties go to the
/// larger one, which scales down better) and converts it. `None` if no entry
/// is usable (no pixmaps, a zero size, or fewer bytes than the size says).
pub(crate) fn convert_best(pixmaps: &[Pixmap]) -> Option<Argb> {
    let target = i32::from(BAR.height);
    pixmaps
        .iter()
        .filter(|(w, h, data)| {
            let needed = usize::try_from(*w)
                .ok()
                .zip(usize::try_from(*h).ok())
                .and_then(|(w, h)| w.checked_mul(h)?.checked_mul(4));
            *w > 0 && *h > 0 && needed.is_some_and(|n| n <= data.len())
        })
        .min_by_key(|(w, _, _)| ((w - target).abs(), -w))
        .and_then(|(w, h, data)| convert(*w, *h, data))
}

/// The four channels of one wire pixel (`[A, R, G, B]`).
const fn channels([a, r, g, b]: [u8; 4]) -> (u8, u8, u8, u8) {
    (a, r, g, b)
}

/// Converts one `width` x `height` pixmap.
fn convert(width: i32, height: i32, data: &[u8]) -> Option<Argb> {
    let count = usize::try_from(width)
        .ok()?
        .checked_mul(usize::try_from(height).ok()?)?;
    let pixels = data.get(..count.checked_mul(4)?)?;
    let recolor = detect_symbolic_mask(pixels).then(|| {
        let text = COLORS.text;
        (channel(text.r), channel(text.g), channel(text.b))
    });
    let mut out = Vec::with_capacity(pixels.len());
    for (a, r, g, b) in pixels.as_chunks::<4>().0.iter().copied().map(channels) {
        let (r, g, b) = recolor.unwrap_or((r, g, b));
        let argb = (u32::from(a) << 24)
            | (u32::from(premultiply(r, a)) << 16)
            | (u32::from(premultiply(g, a)) << 8)
            | u32::from(premultiply(b, a));
        // cairo wants each pixel as a native-endian u32 (0xAARRGGBB).
        out.extend_from_slice(&argb.to_ne_bytes());
    }
    Some(Argb {
        width,
        height,
        data: out,
    })
}

/// `value * alpha / 255`, as the C bar rounds it (down).
fn premultiply(value: u8, alpha: u8) -> u8 {
    u8::try_from(u16::from(value) * u16::from(alpha) / 255).unwrap_or(u8::MAX)
}

/// A 0..1 colour channel as 0..255 (C's `(uint8_t)(c * 255.0 + 0.5)`).
fn channel(value: f64) -> u8 {
    // Every palette channel is n/255 for a whole n, so this finds n exactly
    // without a float-to-int cast.
    (0..=u8::MAX)
        .min_by(|&a, &b| {
            let da = (f64::from(a) - value * 255.0).abs();
            let db = (f64::from(b) - value * 255.0).abs();
            da.total_cmp(&db)
        })
        .unwrap_or(u8::MAX)
}

/// Whether the pixmap looks like a flat-coloured *mask* (e.g. `EasyEffects`'
/// `-symbolic` icon): every sufficiently opaque pixel shares about one dim
/// colour, meant to be recoloured by a theme-aware toolkit. Such an icon is
/// drawn in `COLORS.text` instead, keeping its alpha (the glyph shape).
fn detect_symbolic_mask(pixels: &[u8]) -> bool {
    let mut min = [u8::MAX; 3];
    let mut max = [0_u8; 3];
    let mut any = false;
    for (a, r, g, b) in pixels.as_chunks::<4>().0.iter().copied().map(channels) {
        if a <= SYMBOLIC_ALPHA_THRESHOLD {
            continue;
        }
        any = true;
        for (i, value) in [r, g, b].into_iter().enumerate() {
            if let (Some(lo), Some(hi)) = (min.get_mut(i), max.get_mut(i)) {
                *lo = (*lo).min(value);
                *hi = (*hi).max(value);
            }
        }
    }
    if !any {
        return false;
    }
    let [r, g, b] = [0, 1, 2].map(|i| {
        let (lo, hi) = (
            min.get(i).copied().unwrap_or(0),
            max.get(i).copied().unwrap_or(0),
        );
        (hi - lo, f64::midpoint(f64::from(hi), f64::from(lo)))
    });
    let range = r.0.max(g.0).max(b.0);
    if range > SYMBOLIC_COLOR_TOLERANCE {
        return false;
    }
    // Rec. 601 luma of the shared colour's midpoint, 0..1.
    let luminance = (0.299 * r.1 + 0.587 * g.1 + 0.114 * b.1) / 255.0;
    luminance < SYMBOLIC_LUMINANCE_MAX
}

#[cfg(test)]
mod tests {
    use super::{Pixmap, channel, convert_best, detect_symbolic_mask};
    use crate::config::COLORS;

    /// A `size` x `size` pixmap of one wire pixel.
    fn solid(size: i32, px: [u8; 4]) -> Pixmap {
        let n = usize::try_from(size * size).unwrap();
        (size, size, px.repeat(n))
    }

    #[test]
    fn picks_the_size_closest_to_the_bar() {
        let set = [
            solid(16, [255, 200, 10, 10]),
            solid(24, [255, 200, 10, 10]),
            solid(48, [255, 200, 10, 10]),
        ];
        // 16 and 24 are both 4 px from 20: the larger wins.
        assert_eq!(convert_best(&set).unwrap().width, 24);
        // Short data is skipped.
        assert!(convert_best(&[(16, 16, vec![0; 10])]).is_none());
    }

    #[test]
    fn premultiplies_in_native_order() {
        let argb = convert_best(&[solid(1, [128, 200, 100, 50])]).unwrap();
        let px = u32::from_ne_bytes(argb.data.try_into().unwrap());
        // 200*128/255 = 100, 100*128/255 = 50, 50*128/255 = 25.
        assert_eq!(px, 0x8064_3219);
    }

    #[test]
    fn dim_flat_masks_are_recoloured() {
        // EasyEffects-like: every opaque pixel the same dim grey.
        assert!(detect_symbolic_mask(&[255, 0x30, 0x30, 0x30].repeat(4)));
        // Bright flat colour: left alone.
        assert!(!detect_symbolic_mask(&[255, 0xee, 0xee, 0xee].repeat(4)));
        // Two different colours: a real icon.
        let mut mixed = [255, 0x30, 0x30, 0x30].repeat(2);
        mixed.extend([255, 0x30, 0x90, 0x30]);
        assert!(!detect_symbolic_mask(&mixed));
        // Fully transparent: nothing sampled.
        assert!(!detect_symbolic_mask(&[0, 0, 0, 0].repeat(4)));
        let argb = convert_best(&[solid(1, [255, 0x30, 0x30, 0x30])]).unwrap();
        let px = u32::from_ne_bytes(argb.data.try_into().unwrap());
        let text = COLORS.text;
        let want = (0xff << 24)
            | (u32::from(channel(text.r)) << 16)
            | (u32::from(channel(text.g)) << 8)
            | u32::from(channel(text.b));
        assert_eq!(px, want);
    }
}
