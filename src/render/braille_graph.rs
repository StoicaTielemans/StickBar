//! btop-style "braille" history graphs (the C bar's `render/braille_graph.c`).
//!
//! A grid of character-sized cells, each 2 dots wide and 4 dots tall, with one
//! sample per dot column and each dot coloured by how high it sits. The dots
//! are small cairo circles on the braille geometry rather than braille
//! *characters*: crisp, evenly spaced at any size, and every dot row can have
//! its own colour (characters would need a pango layout per cell). Pure
//! cairo, no data-source types, so any dropdown with a history can use it.

use crate::config::{Color, gradient3};

/// Dots per cell, across.
const DOTS_X: u32 = 2;
/// Dots per cell, down.
const DOTS_Y: u32 = 4;
/// Distance between neighbouring dots.
const DOT_PITCH: f64 = 3.0;
/// Extra space between cells.
const CELL_GAP: f64 = 2.0;
/// Each dot's radius.
const DOT_RADIUS: f64 = 1.0;

/// One cell's width in px (8).
fn cell_w() -> f64 {
    f64::from(DOTS_X) * DOT_PITCH + CELL_GAP
}

/// One cell's height in px (14).
fn cell_h() -> f64 {
    f64::from(DOTS_Y) * DOT_PITCH + CELL_GAP
}

/// A graph `cells` cells wide, in px.
pub(crate) fn width(cells: u32) -> f64 {
    f64::from(cells) * cell_w()
}

/// A graph `cells` cells tall, in px.
pub(crate) fn height(cells: u32) -> f64 {
    f64::from(cells) * cell_h()
}

/// How many whole cells fit in `width` px.
pub(crate) fn cells_for_width(width: f64) -> u32 {
    u32::try_from(crate::render::whole_px((width / cell_w()).floor())).unwrap_or(0)
}

/// The time axis' left label for a graph `cells` cells wide, at one sample per
/// `seconds_per_sample`: e.g. "40 s ago", "8 min ago".
pub(crate) fn span_label(cells: u32, seconds_per_sample: f64) -> String {
    let seconds = f64::from(cells * DOTS_X) * seconds_per_sample;
    if seconds < 90.0 {
        format!("{seconds:.0} s ago")
    } else {
        format!("{:.0} min ago", seconds / 60.0)
    }
}

/// Where dot `index` (counted from the graph's edge) sits, in px from that edge.
fn dot_offset(index: u32, dots_per_cell: u32, cell_size: f64) -> f64 {
    f64::from(index / dots_per_cell) * cell_size
        + CELL_GAP / 2.0
        + f64::from(index % dots_per_cell) * DOT_PITCH
        + DOT_PITCH / 2.0
}

/// Draws `samples` (oldest first) at (x, y) as a graph `cells_w` x `cells_h`
/// cells. The newest sample is the rightmost column; there are no dots where
/// there's no data yet. `scale` is the value of a full column; `grow_up`
/// stacks dots from the bottom. Dots are coloured along `gradient` by their
/// height (row 0 = low end).
pub(crate) fn draw(
    cr: &cairo::Context,
    (x, y): (f64, f64),
    (cells_w, cells_h): (u32, u32),
    samples: &[f64],
    scale: f64,
    grow_up: bool,
    gradient: &[Color; 3],
) -> Result<(), cairo::Error> {
    let (columns, rows) = (cells_w * DOTS_X, cells_h * DOTS_Y);
    let graph_h = height(cells_h);
    if columns == 0 || rows == 0 || scale <= 0.0 {
        return Ok(());
    }
    let columns_usize = usize::try_from(columns).unwrap_or(0);
    // The last `columns` samples, right-aligned: missing ones leave a gap on the left.
    let skip = samples.len().saturating_sub(columns_usize);
    let first_col = columns_usize.saturating_sub(samples.len());
    cr.save()?;
    for (offset, &value) in samples.iter().skip(skip).enumerate() {
        if value <= 0.0 {
            continue;
        }
        let col = u32::try_from(first_col + offset).unwrap_or(0);
        let level = crate::render::whole_px((value / scale * f64::from(rows)).round())
            .clamp(1, i32::try_from(rows).unwrap_or(1));
        let cx = x + dot_offset(col, DOTS_X, cell_w());
        for row in 0..u32::try_from(level).unwrap_or(0) {
            let from_base = dot_offset(row, DOTS_Y, cell_h());
            let cy = if grow_up {
                y + graph_h - from_base
            } else {
                y + from_base
            };
            let t = if rows > 1 {
                f64::from(row) / f64::from(rows - 1)
            } else {
                0.0
            };
            gradient3(gradient, t).set_source(cr);
            cr.new_sub_path();
            cr.arc(cx, cy, DOT_RADIUS, 0.0, std::f64::consts::TAU);
            cr.fill()?;
        }
    }
    cr.restore()
}

// --- The sprite path, for graphs redrawn every frame (the media dropdown's
// spectrum, 15 fps). `draw` fills one antialiased circle per lit dot: fine
// for the network graph's 5 s repaint, thousands per frame for an animated
// one. Instead every dot is rendered once, lit, into an image, and a frame
// copies just the lit part of each column out of it: whole-pixel rectangles,
// since dot centres sit on half pixels 3 px apart, so a column is exactly
// the 3 px band around its centre. Same geometry and colours as `draw`.

/// Dot rows per column, for a graph `cells_h` cells tall.
pub(crate) const fn rows(cells_h: u32) -> u32 {
    cells_h * DOTS_Y
}

/// An image of a `cells_w` x `cells_h` graph with every dot lit, coloured by
/// row like [`draw`]; `grow_up` picks which edge is row 0.
pub(crate) fn render_lit(
    (cells_w, cells_h): (u32, u32),
    grow_up: bool,
    gradient: &[Color; 3],
) -> Result<cairo::ImageSurface, cairo::Error> {
    let w = crate::render::whole_px(width(cells_w).ceil()).max(1);
    let h = crate::render::whole_px(height(cells_h).ceil()).max(1);
    let surface = cairo::ImageSurface::create(cairo::Format::ARgb32, w, h)?;
    let cr = cairo::Context::new(&surface)?;
    let (columns, rows) = (cells_w * DOTS_X, cells_h * DOTS_Y);
    for row in 0..rows {
        let from_base = dot_offset(row, DOTS_Y, cell_h());
        let cy = if grow_up {
            f64::from(h) - from_base
        } else {
            from_base
        };
        let t = if rows > 1 {
            f64::from(row) / f64::from(rows - 1)
        } else {
            0.0
        };
        gradient3(gradient, t).set_source(&cr);
        for col in 0..columns {
            cr.new_sub_path();
            cr.arc(
                dot_offset(col, DOTS_X, cell_w()),
                cy,
                DOT_RADIUS,
                0.0,
                std::f64::consts::TAU,
            );
        }
        // One fill per row: a row shares its colour.
        cr.fill()?;
    }
    drop(cr);
    surface.flush();
    Ok(surface)
}

/// A rectangle in whole pixels: (x, y, w, h).
pub(crate) type PixelRect = (i32, i32, i32, i32);

/// Where dot rows `rows.0..rows.1` of column `col` are, graph-local whole
/// pixels: copying exactly this out of a [`render_lit`] image (or a panel
/// with one composited onto it) lights those dots and nothing else.
pub(crate) fn column_rect(
    cells_h: u32,
    col: u32,
    (row_from, row_to): (u32, u32),
    grow_up: bool,
) -> PixelRect {
    let graph_h = height(cells_h);
    let cx = dot_offset(col, DOTS_X, cell_w());
    // Each dot's centre +- half the pitch: the band covers whole dots and
    // none of the next, and every edge lands on a whole pixel.
    let near = dot_offset(row_from, DOTS_Y, cell_h()) - DOT_PITCH / 2.0;
    let far = dot_offset(row_to.saturating_sub(1), DOTS_Y, cell_h()) + DOT_PITCH / 2.0;
    let px = |v: f64| crate::render::whole_px(v.round());
    (
        px(cx - DOT_PITCH / 2.0),
        px(if grow_up { graph_h - far } else { near }),
        px(DOT_PITCH),
        px(far - near),
    )
}

#[cfg(test)]
mod tests {
    use super::{cells_for_width, column_rect, span_label, width};

    #[test]
    fn column_rects() {
        // Column 0, the bottom dot row of a 4-cell graph growing up: dot
        // centre (2.5, 56 - 2.5) +- 1.5.
        assert_eq!(column_rect(4, 0, (0, 1), true), (1, 52, 3, 3));
        // Column 3 (second cell's 2nd column), rows 0..4 hanging down: one
        // whole cell's dots, 12 px.
        assert_eq!(column_rect(4, 3, (0, 4), false), (12, 1, 3, 12));
        // Rows 0..16 cross the cell gaps: 4 cells' worth minus the end gaps.
        assert_eq!(column_rect(4, 0, (0, 16), true), (1, 1, 3, 54));
    }

    #[test]
    fn geometry() {
        assert_eq!(width(30), 240.0);
        assert_eq!(cells_for_width(247.9), 30);
        assert_eq!(cells_for_width(-5.0), 0);
    }

    #[test]
    fn span_labels() {
        assert_eq!(span_label(4, 5.0), "40 s ago");
        assert_eq!(span_label(48, 5.0), "8 min ago");
    }
}
