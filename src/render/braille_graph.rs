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

#[cfg(test)]
mod tests {
    use super::{cells_for_width, span_label, width};

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
