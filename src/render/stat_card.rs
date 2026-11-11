//! The parts the CPU, GPU, memory and network dropdowns share (the C bar's
//! `render/stat_card.c`): the graph card's sizing, the history window it
//! shows, its label and time-axis rows, label/value rows, and the
//! "top apps" rows.

use super::{BoxRect, braille_graph, meter_bar, popup_style::COL_GAP, text::TextCache};
use crate::config::{COLORS, Color, FONT};

/// Graph card border to its contents.
pub(crate) const CARD_PAD: f64 = 8.0;
/// The "used 12%   peak 40%" row above a graph.
pub(crate) const GRAPH_LABEL_H: f64 = 14.0;
/// Label row to graph.
pub(crate) const GRAPH_LABEL_GAP: f64 = 3.0;
/// Between two graphs in one card.
pub(crate) const GRAPH_SPLIT_GAP: f64 = 8.0;
/// A graph's minimum width in cells: 5 minutes of 5 s samples.
pub(crate) const GRAPH_MIN_CELLS_W: u32 = 30;
/// One label/value row.
pub(crate) const ROW_H: f64 = 17.0;
/// The bar in a "top apps" row.
pub(crate) const APP_BAR_W: f64 = 70.0;
/// That bar's height.
const APP_BAR_H: f64 = 5.0;
/// The time axis row under a graph card ("8 min ago ... now").
const AXIS_H: f64 = 12.0;
/// Card bottom to axis text.
const AXIS_OFFSET: f64 = 2.0;
/// One history sample per 5 s poll.
const SECONDS_PER_SAMPLE: f64 = 5.0;
/// Segments in a "top apps" bar.
const APP_BAR_SEGMENTS: u32 = 7;

/// How many cells wide a graph in a `card_w` px card is: as many as fit, up
/// to half the history (two samples per cell). Graphs fill their card, so the
/// newest dots reach its right border (BUG.md, "Graphs stopped short").
pub(crate) fn graph_cells(card_w: f64, history_capacity: usize) -> u32 {
    let most = u32::try_from(history_capacity / 2).unwrap_or(u32::MAX);
    braille_graph::cells_for_width(card_w - CARD_PAD * 2.0).min(most)
}

/// Which samples a `cells` wide graph shows of a history holding `len`:
/// (index of the first, how many), the newest `cells * 2` at most.
pub(crate) fn history_window(len: usize, cells: u32) -> (usize, usize) {
    let shown = usize::try_from(cells).unwrap_or(0) * 2;
    let first = len.saturating_sub(shown);
    (first, len - first)
}

/// A graph's label row: `left` in bold (e.g. "used 41%") in `left_color`,
/// `right` (e.g. "peak 57%") small and dim, right-aligned.
pub(crate) fn graph_label_row(
    cr: &cairo::Context,
    text: &mut TextCache,
    (x, y, w): (f64, f64, f64),
    left: &str,
    left_color: Color,
    right: &str,
) -> Result<(), cairo::Error> {
    text.draw_left(cr, (x, y, GRAPH_LABEL_H), left, FONT.sm, true, left_color)?;
    text.draw_right(
        cr,
        (x + w, y, GRAPH_LABEL_H),
        right,
        FONT.xs,
        false,
        COLORS.overlay0,
    )
}

/// The time axis row's height, card bottom to the next section.
pub(crate) const fn axis_height() -> f64 {
    AXIS_H + AXIS_OFFSET
}

/// The time axis under a graph card at y: how far back the graph reaches on
/// the left, "now" on the right.
pub(crate) fn draw_axis(
    cr: &cairo::Context,
    text: &mut TextCache,
    (x, y, w): (f64, f64, f64),
    cells: u32,
) -> Result<(), cairo::Error> {
    let span = braille_graph::span_label(cells, SECONDS_PER_SAMPLE);
    let row_y = y + AXIS_OFFSET;
    text.draw_left(
        cr,
        (x, row_y, AXIS_H),
        &span,
        FONT.xs,
        false,
        COLORS.overlay0,
    )?;
    text.draw_right(
        cr,
        (x + w, row_y, AXIS_H),
        "now",
        FONT.xs,
        false,
        COLORS.overlay0,
    )
}

/// The widest of `labels` (small bold), for a rows' label column.
pub(crate) fn label_col_w(text: &mut TextCache, labels: &[&str]) -> f64 {
    labels
        .iter()
        .map(|label| text.width(label, FONT.xs, true))
        .fold(0.0, f64::max)
}

/// A "top apps" row's name column: room for a 15-character process name.
pub(crate) fn app_name_col_w(text: &mut TextCache) -> f64 {
    text.width("000000000000000", FONT.label, false) + COL_GAP
}

/// A "top apps" row: name, a bar `fraction` full (relative to the biggest),
/// and the value right-aligned.
pub(crate) fn draw_app_row(
    cr: &cairo::Context,
    text: &mut TextCache,
    (x, y, w): (f64, f64, f64),
    name: &str,
    fraction: f64,
    bar_color: Color,
    value: &str,
) -> Result<(), cairo::Error> {
    text.draw_left(cr, (x, y, ROW_H), name, FONT.label, false, COLORS.text)?;
    let bar = BoxRect {
        x: x + app_name_col_w(text),
        y: y + (ROW_H - APP_BAR_H) / 2.0,
        w: APP_BAR_W,
        h: APP_BAR_H,
    };
    meter_bar::draw(
        cr,
        bar,
        fraction,
        APP_BAR_SEGMENTS,
        &[bar_color; 3],
        COLORS.surface0,
    )?;
    text.draw_right(
        cr,
        (x + w, y, ROW_H),
        value,
        FONT.label,
        false,
        COLORS.subtext0,
    )
}

#[cfg(test)]
mod tests {
    use super::{graph_cells, history_window};

    #[test]
    fn graph_fills_its_card_up_to_half_the_history() {
        assert_eq!(graph_cells(256.0, 120), 30); // (256 - 16) / 8
        assert_eq!(graph_cells(10_000.0, 120), 60);
    }

    #[test]
    fn window_is_the_newest_samples() {
        assert_eq!(history_window(120, 30), (60, 60));
        assert_eq!(history_window(3, 30), (0, 3));
    }
}
