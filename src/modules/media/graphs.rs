//! The media dropdown's spectrum graphs: one per card (the first
//! [`MAX_SOURCES`]), each on its own strip subsurface (the graph half of the
//! C bar's `modules/media/media_popup.c`; `docs/modules/media.md`).
//!
//! Drawn like the network graph: braille dots, the left channel rising toward
//! a centre line and the right hanging below it, one dot column per band,
//! peak dots, soft faded ends. **A frame is plain byte copies, no cairo.**
//! Three whole panels are rendered once per open (empty; every dot lit; every
//! dot in the peak colour, each with the fade baked in), and a frame copies
//! the empty panel, then per column the lit rows' rectangle out of the lit
//! panel and the peak dot's out of the peak one, straight into the strip's
//! shm buffer. A frame where no dot changed isn't drawn at all.

use smithay_client_toolkit::reexports::client::{QueueHandle, protocol::wl_surface::WlSurface};

use super::Media;
use crate::{
    bar::strip_surface::{StripEnv, StripSurface},
    config::{COLORS, Color},
    render::{braille_graph, whole_px},
    visualizer::{Levels, MAX_SOURCES, spectrum::BANDS},
    wayland::App,
};

/// Braille cells across: 41 x 8 px = 328 of the 340 px panel, 82 dot columns.
const VIS_CELLS: u32 = 41;
/// Cells per half (like the network graph's).
const VIS_CELLS_H: u32 = 4;
/// Panel edge to the graph.
const VIS_PAD_Y: f64 = 6.0;
/// Between the halves; the centre line runs through it.
const VIS_CENTER_GAP: f64 = 5.0;
/// The panel's corner radius.
const VIS_RADIUS: f64 = 4.0;
/// The soft ends' width.
const VIS_FADE_W: f64 = 28.0;
/// Card text to the graph panel.
pub(crate) const VIS_ROW_GAP: f64 = 10.0;

/// One half's height (4 cells of 14 px).
fn half_h() -> f64 {
    braille_graph::height(VIS_CELLS_H)
}

/// The whole panel's height (129 px).
pub(crate) fn row_h() -> f64 {
    2.0 * VIS_PAD_Y + 2.0 * half_h() + VIS_CENTER_GAP
}

/// Dot rows per half (16).
const ROWS: u32 = braille_graph::rows(VIS_CELLS_H);

/// Where the graph starts inside a panel `panel_w` wide.
fn graph_x(panel_w: f64) -> f64 {
    ((panel_w - braille_graph::width(VIS_CELLS)) / 2.0).floor()
}

/// The left half's top, panel-local.
const fn top_y() -> f64 {
    VIS_PAD_Y
}

/// The right half's top, panel-local.
fn bottom_y() -> f64 {
    VIS_PAD_Y + half_h() + VIS_CENTER_GAP
}

/// The geometry the tests' `dump visualizer` reports for a panel at
/// (`x`, `y`), `w` wide: (`graph_x`, `top_y`, `bottom_y`, `half_h`, `rows`).
pub(crate) fn test_geometry(x: f64, y: f64, w: f64) -> (f64, f64, f64, f64, u32) {
    (x + graph_x(w), y + top_y(), y + bottom_y(), half_h(), ROWS)
}

/// One frame in dot rows per channel: how many dots each column has lit, and
/// its peak dot's row (0 = none). Compared whole: an unchanged frame isn't drawn.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct VisFrame {
    /// Lit dots per column, per channel.
    level: [[u8; BANDS]; 2],
    /// The peak dot's row + 1 per column (0 = none), per channel.
    peak: [[u8; BANDS]; 2],
}

/// A 0..1 value as 0..=`ROWS` dots.
fn dots(value: f32) -> u8 {
    let rows = f64::from(ROWS);
    u8::try_from(whole_px((f64::from(value) * rows).round()).clamp(0, 255)).unwrap_or(0)
}

impl VisFrame {
    /// The frame for these levels and peaks.
    fn of(levels: &Levels, peaks: &Levels) -> Self {
        let mut frame = Self {
            level: [[0; BANDS]; 2],
            peak: [[0; BANDS]; 2],
        };
        for (((out_level, out_peak), channel_levels), channel_peaks) in frame
            .level
            .iter_mut()
            .zip(frame.peak.iter_mut())
            .zip(levels)
            .zip(peaks)
        {
            for (((l, p), &level), &peak) in out_level
                .iter_mut()
                .zip(out_peak.iter_mut())
                .zip(channel_levels)
                .zip(channel_peaks)
            {
                *l = dots(level);
                // A peak dot only where it sits clear of the column: resting
                // on top it would just look like one more lit dot.
                let pk = dots(peak);
                *p = if pk > *l { pk } else { 0 };
            }
        }
        frame
    }
}

/// The three pre-rendered panels' bytes (ARGB32, `w * 4` per row).
struct Panels {
    /// Width in px.
    w: usize,
    /// Height in px.
    h: usize,
    /// The empty panel.
    bg: Vec<u8>,
    /// Every dot lit.
    lit: Vec<u8>,
    /// Every dot in the peak colour.
    peak: Vec<u8>,
}

/// Renders one whole panel `w` x `h`: the card's fill around the rounded
/// corners, the mantle panel and its border, the centre line, optionally
/// every dot (`colors` = left gradient, right gradient), then the fade.
fn render_panel(
    (w, h): (i32, i32),
    colors: Option<&[[Color; 3]; 2]>,
) -> Result<Vec<u8>, cairo::Error> {
    let mut surface = cairo::ImageSurface::create(cairo::Format::ARgb32, w, h)?;
    // Its own function, so the context is gone before the pixels are read.
    draw_panel(&surface, (f64::from(w), f64::from(h)), colors)?;
    surface.flush();
    let stride = surface.stride();
    let data = surface
        .data()
        .map_err(|_borrowed| cairo::Error::SurfaceFinished)?;
    if stride != w * 4 {
        return Err(cairo::Error::InvalidStride);
    }
    Ok(data.to_vec())
}

/// Draws a panel's picture onto `surface` (see [`render_panel`]).
fn draw_panel(
    surface: &cairo::ImageSurface,
    (fw, fh): (f64, f64),
    colors: Option<&[[Color; 3]; 2]>,
) -> Result<(), cairo::Error> {
    let cr = cairo::Context::new(surface)?;
    COLORS.base.set_source(&cr);
    cr.paint()?;
    rounded_rect(&cr, (0.5, 0.5, fw - 1.0, fh - 1.0), VIS_RADIUS);
    COLORS.mantle.set_source(&cr);
    cr.fill_preserve()?;
    COLORS.surface0.set_source(&cr);
    cr.set_line_width(1.0);
    cr.stroke()?;
    let gx = graph_x(fw);
    COLORS.surface1.set_source(&cr);
    cr.rectangle(
        gx,
        (VIS_PAD_Y + half_h() + VIS_CENTER_GAP / 2.0).floor(),
        braille_graph::width(VIS_CELLS),
        1.0,
    );
    cr.fill()?;
    if let Some(colors) = colors {
        for ((gradient, grow_up), y) in colors.iter().zip([true, false]).zip([top_y(), bottom_y()])
        {
            let dots = braille_graph::render_lit((VIS_CELLS, VIS_CELLS_H), grow_up, gradient)?;
            cr.set_source_surface(&dots, gx, y)?;
            cr.paint()?;
        }
    }
    paint_fade(&cr, fw, fh)
}

/// A rounded rectangle path.
fn rounded_rect(
    cr: &cairo::Context,
    (left, top, width, height): (f64, f64, f64, f64),
    radius: f64,
) {
    use std::f64::consts::{FRAC_PI_2, PI};
    let (right, bottom) = (left + width, top + height);
    cr.new_sub_path();
    cr.arc(right - radius, top + radius, radius, -FRAC_PI_2, 0.0);
    cr.arc(right - radius, bottom - radius, radius, 0.0, FRAC_PI_2);
    cr.arc(left + radius, bottom - radius, radius, FRAC_PI_2, PI);
    cr.arc(left + radius, top + radius, radius, PI, 3.0 * FRAC_PI_2);
    cr.close_path();
}

/// The soft left and right ends: mantle fading in over [`VIS_FADE_W`],
/// inside the 1 px border.
fn paint_fade(cr: &cairo::Context, w: f64, h: f64) -> Result<(), cairo::Error> {
    let m = COLORS.mantle;
    for (x0, a0, a1) in [(1.0, 1.0, 0.0), (w - 1.0 - VIS_FADE_W, 0.0, 1.0)] {
        let gradient = cairo::LinearGradient::new(x0, 0.0, x0 + VIS_FADE_W, 0.0);
        gradient.add_color_stop_rgba(0.0, m.r, m.g, m.b, a0);
        gradient.add_color_stop_rgba(1.0, m.r, m.g, m.b, a1);
        cr.set_source(&gradient)?;
        cr.rectangle(x0, 1.0, VIS_FADE_W, h - 2.0);
        cr.fill()?;
    }
    Ok(())
}

impl Panels {
    /// Renders the three panels, `w` x `h`.
    fn render(w: i32, h: i32) -> Result<Self, cairo::Error> {
        let lit = [
            [COLORS.yellow, COLORS.peach, COLORS.red], // left, rising
            [COLORS.mauve, COLORS.pink, COLORS.red],   // right, hanging
        ];
        let peak = [[COLORS.text; 3], [COLORS.text; 3]];
        Ok(Self {
            w: usize::try_from(w).unwrap_or(0),
            h: usize::try_from(h).unwrap_or(0),
            bg: render_panel((w, h), None)?,
            lit: render_panel((w, h), Some(&lit))?,
            peak: render_panel((w, h), Some(&peak))?,
        })
    }

    /// Writes `frame` into `dst` (a whole panel, `w * 4` per row). Returns
    /// false if `dst` isn't panel-sized.
    fn compose(&self, frame: &VisFrame, dst: &mut [u8]) -> bool {
        if dst.len() != self.bg.len() {
            return false;
        }
        dst.copy_from_slice(&self.bg);
        let gx = whole_px(graph_x(f64::from(u32::try_from(self.w).unwrap_or(0))));
        let half_tops = [whole_px(top_y()), whole_px(bottom_y())];
        for (((levels, peaks), grow_up), half_y) in frame
            .level
            .iter()
            .zip(&frame.peak)
            .zip([true, false])
            .zip(half_tops)
        {
            for ((col, &level), &peak) in (0_u32..).zip(levels).zip(peaks) {
                if level > 0 {
                    let rect = braille_graph::column_rect(
                        VIS_CELLS_H,
                        col,
                        (0, u32::from(level)),
                        grow_up,
                    );
                    self.copy(&self.lit, dst, rect, (gx, half_y));
                }
                if peak > 0 {
                    let p = u32::from(peak);
                    let rect = braille_graph::column_rect(VIS_CELLS_H, col, (p - 1, p), grow_up);
                    self.copy(&self.peak, dst, rect, (gx, half_y));
                }
            }
        }
        true
    }

    /// Copies graph-local `rect` (offset by `origin` into the panel) from
    /// `src` to `dst`, row by row.
    fn copy(
        &self,
        src: &[u8],
        dst: &mut [u8],
        (x, y, w, h): braille_graph::PixelRect,
        origin: (i32, i32),
    ) {
        let stride = self.w * 4;
        let (Ok(x), Ok(y), Ok(w), Ok(h)) = (
            usize::try_from(x + origin.0),
            usize::try_from(y + origin.1),
            usize::try_from(w),
            usize::try_from(h),
        ) else {
            return;
        };
        for row in y..(y + h).min(self.h) {
            let start = row * stride + x * 4;
            let range = start..start + w * 4;
            if let (Some(d), Some(s)) = (dst.get_mut(range.clone()), src.get(range)) {
                d.copy_from_slice(s);
            }
        }
    }
}

/// One card's graph.
#[derive(Default)]
struct CardGraph {
    /// Its strip, made on the current popup surface.
    strip: Option<StripSurface>,
    /// Placed and drawn at least once.
    shown: bool,
    /// Where it was last placed (popup-local y).
    placed_y: f64,
    /// The frame it shows.
    drawn: Option<VisFrame>,
}

/// Every card's graph, and the panels they're drawn from.
#[derive(Default)]
pub(crate) struct Graphs {
    /// One per slot.
    cards: [CardGraph; MAX_SOURCES],
    /// Rendered on first use, per open.
    panels: Option<Panels>,
    /// A panel's (x, width) in the popup.
    panel: (f64, f64),
}

impl Graphs {
    /// Places and draws the graphs after a (re)layout: `rows` is each slot's
    /// panel top (popup-local), `None` for a card without a graph. Strips are
    /// (re)made on `parent`; without `env` (no subcompositor) nothing is drawn.
    pub(crate) fn update(
        &mut self,
        env: Option<&StripEnv<'_>>,
        parent: &WlSurface,
        (panel_x, panel_w): (f64, f64),
        rows: &[Option<f64>],
        media: &Media,
    ) {
        self.panel = (panel_x, panel_w);
        let size = (whole_px(panel_w), whole_px(row_h()));
        if self.panels.is_none() {
            self.panels = Panels::render(size.0, size.1)
                .map_err(|err| eprintln!("stickBar: rendering the visualizer panels failed: {err}"))
                .ok();
        }
        for (slot, graph) in self.cards.iter_mut().enumerate() {
            if graph.strip.as_ref().is_some_and(|s| s.parent() != parent) {
                // The popup surface was recreated: its strips went with it.
                graph.strip = None;
                graph.shown = false;
            }
            let (Some(env), Some(vis_y)) = (env, rows.get(slot).copied().flatten()) else {
                if let Some(strip) = graph.strip.as_mut() {
                    strip.hide();
                }
                graph.shown = false;
                continue;
            };
            let strip = graph
                .strip
                .get_or_insert_with(|| StripSurface::new(env, parent));
            let frame = frame_for(media, slot);
            let moved = !graph.shown || (graph.placed_y - vis_y).abs() > 0.5;
            if moved {
                strip.place(env.shm, (whole_px(panel_x), whole_px(vis_y)), size);
                graph.placed_y = vis_y;
            }
            if moved || graph.drawn != Some(frame) {
                graph.drawn = Some(frame);
                draw_strip(strip, env.qh, self.panels.as_ref(), &frame);
            }
            graph.shown = true;
        }
    }

    /// (shown, ready) slot masks for the visualizer's pump: a graph on
    /// screen, and one whose strip can take a new frame now.
    pub(crate) fn masks(&self) -> (u32, u32) {
        let mut masks = (0, 0);
        for (i, graph) in self.cards.iter().enumerate() {
            let bit = 1_u32 << i;
            if graph.shown {
                masks.0 |= bit;
                if graph.strip.as_ref().is_some_and(StripSurface::ready) {
                    masks.1 |= bit;
                }
            }
        }
        masks
    }

    /// Whether slot `slot`'s graph is on screen.
    pub(crate) fn shown(&self, slot: usize) -> bool {
        self.cards.get(slot).is_some_and(|g| g.shown)
    }

    /// Redraws the graphs of the slots in `changed` whose dots moved. Only
    /// their own strips are touched: the popup's text isn't.
    pub(crate) fn draw_changed(&mut self, changed: u32, qh: &QueueHandle<App>, media: &Media) {
        for (slot, graph) in self.cards.iter_mut().enumerate() {
            if changed & (1 << slot) == 0 || !graph.shown {
                continue;
            }
            let frame = frame_for(media, slot);
            if graph.drawn == Some(frame) {
                continue; // no dot changed: no commit
            }
            graph.drawn = Some(frame);
            if let Some(strip) = graph.strip.as_mut() {
                draw_strip(strip, qh, self.panels.as_ref(), &frame);
            }
        }
    }

    /// A frame callback for `surface`, if it's one of these strips: a draw
    /// that waited for it goes out now. Returns whether it was one.
    pub(crate) fn frame_done(&mut self, surface: &WlSurface, qh: &QueueHandle<App>) -> bool {
        let panels = self.panels.as_ref();
        for graph in &mut self.cards {
            let Some(strip) = graph.strip.as_mut().filter(|s| s.is(surface)) else {
                continue;
            };
            if strip.frame_done()
                && let Some(frame) = graph.drawn
            {
                draw_strip(strip, qh, panels, &frame);
            }
            return true;
        }
        false
    }

    /// Paints slot `slot`'s current frame into the popup's own picture at
    /// popup-local `y`, under its strip: invisible there, but it's what makes
    /// the tests' `snapshot` show the graph.
    pub(crate) fn paint_under(
        &self,
        cr: &cairo::Context,
        slot: usize,
        y: f64,
        media: &Media,
    ) -> Result<(), cairo::Error> {
        let Some(panels) = self.panels.as_ref() else {
            return Ok(());
        };
        let mut pixels = vec![0; panels.bg.len()];
        if !panels.compose(&frame_for(media, slot), &mut pixels) {
            return Ok(());
        }
        let (w, h) = (
            i32::try_from(panels.w).unwrap_or(0),
            i32::try_from(panels.h).unwrap_or(0),
        );
        let image =
            cairo::ImageSurface::create_for_data(pixels, cairo::Format::ARgb32, w, h, w * 4)?;
        cr.set_source_surface(&image, self.panel.0, y)?;
        cr.paint()
    }
}

/// Slot `slot`'s current frame.
fn frame_for(media: &Media, slot: usize) -> VisFrame {
    let none = [[0.0; BANDS]; 2];
    let vis = &media.visualizer;
    VisFrame::of(
        vis.levels(slot).unwrap_or(&none),
        vis.peaks(slot).unwrap_or(&none),
    )
}

/// Draws `frame` on `strip`, composed from `panels`.
fn draw_strip(
    strip: &mut StripSurface,
    qh: &QueueHandle<App>,
    panels: Option<&Panels>,
    frame: &VisFrame,
) {
    let Some(panels) = panels else {
        return;
    };
    strip.draw(qh, |canvas| panels.compose(frame, canvas));
}
