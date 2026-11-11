//! Text through pango, with a cache of shaped layouts (the text half of the C
//! bar's `render/module_box.c`).
//!
//! Shaping text (turning a string into positioned glyphs, with font fallback
//! for icons and kanji) is the expensive part of drawing a bar. Most strings
//! don't change between redraws, so each shaped `pango::Layout` is kept and
//! reused: measuring a string and then drawing it shapes it once, and a string
//! unchanged since the last redraw isn't shaped again at all.
//!
//! Every piece of text in the bar goes through here. cairo's own "toy" text
//! API can't shape Nerd Font icons or CJK characters, and is a disallowed method
//! (`clippy.toml`).

use std::collections::HashMap;

use super::BoxRect;
use crate::config::{Color, FONT};

/// How many shaped layouts are kept (the same limit as the C bar). The least
/// recently used one is dropped to make room.
const LAYOUT_CACHE_SIZE: usize = 64;

/// One shaped string at one font size and weight.
#[derive(Debug)]
struct CachedLayout {
    /// Font size in px this layout was shaped at.
    size: i32,
    /// Whether it was shaped bold.
    bold: bool,
    /// The shaped text, ready to draw.
    layout: pango::Layout,
    /// Logical width in px (the advance width: where the next text would start).
    width: f64,
    /// Logical height in px.
    height: f64,
    /// When this was last used, on `TextCache::clock`, for least-recently-used eviction.
    used: u64,
}

/// The shared pango state and the layout cache.
#[derive(Debug)]
pub(crate) struct TextCache {
    /// The one pango context every layout is made from. Created once from a
    /// throwaway 1x1 cairo image, so it carries cairo's default font options and
    /// resolution. Layouts made from it can be drawn onto any cairo context.
    context: pango::Context,
    /// Font descriptions already built, one per (size, bold).
    fonts: Vec<(i32, bool, pango::FontDescription)>,
    /// Shaped layouts, keyed by their text. A string can be cached at several
    /// sizes/weights, hence the inner list. Keying by the text means a lookup
    /// needs only a `&str` and never allocates.
    layouts: HashMap<String, Vec<CachedLayout>>,
    /// How many layouts are cached in total, across every inner list.
    count: usize,
    /// A counter bumped on every lookup, used as the "last used" timestamp.
    clock: u64,
}

impl TextCache {
    /// Creates the shared pango context.
    pub(crate) fn new() -> Result<Self, cairo::Error> {
        let surface = cairo::ImageSurface::create(cairo::Format::ARgb32, 1, 1)?;
        let cr = cairo::Context::new(&surface)?;
        let context = pangocairo::functions::create_context(&cr);
        Ok(Self {
            context,
            fonts: Vec::new(),
            layouts: HashMap::new(),
            count: 0,
            clock: 0,
        })
    }

    /// The logical width in px of `text` at `size` px, bold or not.
    pub(crate) fn width(&mut self, text: &str, size: i32, bold: bool) -> f64 {
        self.layout(text, size, bold)
            .map_or(0.0, |cached| cached.width)
    }

    /// The logical (width, height) in px of `text`, for laying out around it.
    pub(crate) fn size(&mut self, text: &str, size: i32, bold: bool) -> (f64, f64) {
        self.layout(text, size, bold)
            .map_or((0.0, 0.0), |cached| (cached.width, cached.height))
    }

    /// The *ink* width in px of `text`: the pixels the glyphs actually cover,
    /// rather than the logical (advance) width. For most text the two are
    /// close, but the Arch logo glyph draws 14px of ink on an 8px advance, and
    /// a box sized from the logical width cut it off (BUG.md, "Arch logo cut off").
    pub(crate) fn ink_width(&mut self, text: &str, size: i32, bold: bool) -> f64 {
        self.layout(text, size, bold).map_or(0.0, |cached| {
            let (ink, _logical) = cached.layout.pixel_extents();
            f64::from(ink.width())
        })
    }

    /// `text`, shortened to fit `max_w` px: the longest prefix (cut at a
    /// character boundary, never inside one) plus `...`, or `text` itself if
    /// it fits. Found by binary search. The candidates are measured on a
    /// throwaway layout so they don't push real entries out of the cache.
    pub(crate) fn truncate(&mut self, text: &str, max_w: f64, size: i32, bold: bool) -> String {
        if self.width(text, size, bold) <= max_w {
            return text.to_owned();
        }
        // Byte offsets of every character boundary, including the end.
        let bounds: Vec<usize> = text
            .char_indices()
            .map(|(i, _)| i)
            .chain(std::iter::once(text.len()))
            .collect();
        let shortened = |chars: usize| {
            let prefix = bounds
                .get(chars)
                .and_then(|&end| text.get(..end))
                .unwrap_or_default();
            format!("{prefix}...")
        };
        let scratch = pango::Layout::new(&self.context);
        scratch.set_font_description(Some(&self.font(size, bold)));
        let fits = |candidate: &str| {
            scratch.set_text(candidate);
            f64::from(scratch.pixel_size().0) <= max_w
        };
        // Keep `low` chars: always fits (or is 0); `high` is the most it could be.
        let (mut low, mut high) = (0, bounds.len().saturating_sub(1));
        while low < high {
            let mid = (low + high).div_ceil(2);
            if fits(&shortened(mid)) {
                low = mid;
            } else {
                high = mid - 1;
            }
        }
        shortened(low)
    }

    /// Draws `text` ending at `right_x`, vertically centered in the row `y..y + h`.
    pub(crate) fn draw_right(
        &mut self,
        cr: &cairo::Context,
        (right_x, y, h): (f64, f64, f64),
        text: &str,
        size: i32,
        bold: bool,
        color: Color,
    ) -> Result<(), cairo::Error> {
        let Some(cached) = self.layout(text, size, bold) else {
            return Ok(());
        };
        show(
            cr,
            cached,
            right_x - cached.width,
            y + (h - cached.height) / 2.0,
            color,
        )
    }

    /// Draws `text` starting at `x`, vertically centered in the row `y..y + h`.
    pub(crate) fn draw_left(
        &mut self,
        cr: &cairo::Context,
        (x, y, h): (f64, f64, f64),
        text: &str,
        size: i32,
        bold: bool,
        color: Color,
    ) -> Result<(), cairo::Error> {
        let Some(cached) = self.layout(text, size, bold) else {
            return Ok(());
        };
        show(cr, cached, x, y + (h - cached.height) / 2.0, color)
    }

    /// Draws `text` centered in `rect`.
    pub(crate) fn draw_centered(
        &mut self,
        cr: &cairo::Context,
        rect: BoxRect,
        text: &str,
        size: i32,
        bold: bool,
        color: Color,
    ) -> Result<(), cairo::Error> {
        let Some(cached) = self.layout(text, size, bold) else {
            return Ok(()); // nothing shaped, nothing to draw
        };
        let x = rect.x + (rect.w - cached.width) / 2.0;
        show(
            cr,
            cached,
            x,
            rect.y + (rect.h - cached.height) / 2.0,
            color,
        )
    }

    /// The cached layout for (text, size, bold), shaping it first if needed.
    /// `None` only if the cache somehow lost what it just inserted.
    fn layout(&mut self, text: &str, size: i32, bold: bool) -> Option<&CachedLayout> {
        self.clock += 1;
        let now = self.clock;

        let hit = self
            .layouts
            .get_mut(text)
            .and_then(|list| list.iter_mut().find(|c| c.size == size && c.bold == bold));
        if let Some(cached) = hit {
            cached.used = now;
        } else {
            if self.count >= LAYOUT_CACHE_SIZE {
                self.evict_oldest();
            }
            let cached = self.shape(text, size, bold, now);
            self.layouts
                .entry(text.to_owned())
                .or_default()
                .push(cached);
            self.count += 1;
        }

        self.layouts
            .get(text)?
            .iter()
            .find(|c| c.size == size && c.bold == bold)
    }

    /// Shapes `text` into a new layout.
    fn shape(&mut self, text: &str, size: i32, bold: bool, now: u64) -> CachedLayout {
        let layout = pango::Layout::new(&self.context);
        layout.set_font_description(Some(&self.font(size, bold)));
        layout.set_text(text);
        let (width, height) = layout.pixel_size();
        CachedLayout {
            size,
            bold,
            layout,
            width: f64::from(width),
            height: f64::from(height),
            used: now,
        }
    }

    /// The font description for (size, bold), built once and kept. Returned
    /// as a copy: it's only needed when shaping a new string (a cache miss),
    /// so the copy is rare and cheap.
    fn font(&mut self, size: i32, bold: bool) -> pango::FontDescription {
        if let Some((_, _, desc)) = self.fonts.iter().find(|(s, b, _)| *s == size && *b == bold) {
            return desc.clone();
        }
        let mut desc = pango::FontDescription::new();
        desc.set_family(FONT.family);
        desc.set_weight(if bold {
            pango::Weight::Bold
        } else {
            pango::Weight::Normal
        });
        // Absolute size: pixels, not points, so it's independent of DPI.
        desc.set_absolute_size(f64::from(size) * f64::from(pango::SCALE));
        self.fonts.push((size, bold, desc.clone()));
        desc
    }

    /// Drops the least recently used layout.
    fn evict_oldest(&mut self) {
        let oldest = self
            .layouts
            .iter()
            .flat_map(|(text, list)| list.iter().map(move |c| (c.used, text)))
            .min_by_key(|(used, _)| *used)
            .map(|(used, text)| (used, text.clone()));
        let Some((used, text)) = oldest else {
            return;
        };
        if let Some(list) = self.layouts.get_mut(&text) {
            list.retain(|c| c.used != used);
            if list.is_empty() {
                let _removed: Option<Vec<CachedLayout>> = self.layouts.remove(&text);
            }
        }
        self.count -= 1;
    }
}

/// Draws a shaped layout with its top-left corner at (x, y).
fn show(
    cr: &cairo::Context,
    cached: &CachedLayout,
    x: f64,
    y: f64,
    color: Color,
) -> Result<(), cairo::Error> {
    cr.save()?;
    color.set_source(cr);
    cr.move_to(x, y);
    pangocairo::functions::show_layout(cr, &cached.layout);
    cr.restore()
}

#[cfg(test)]
mod tests {
    use super::TextCache;

    #[test]
    fn truncate_keeps_what_fits_and_never_splits_a_character() {
        let mut text = TextCache::new().expect("pango");
        let full = "Tuesday, 3 November \u{b7} caf\u{e9}";
        assert_eq!(text.truncate(full, 10_000.0, 11, false), full);
        let width = text.width("Tuesday...", 11, false);
        let cut = text.truncate(full, width, 11, false);
        assert_eq!(cut, "Tuesday...");
        // Even with no room at all, the result is valid UTF-8 ending in "...".
        assert_eq!(text.truncate(full, 0.0, 11, false), "...");
    }
}
