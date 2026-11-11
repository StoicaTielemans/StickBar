//! Workspace pills (the C bar's `modules/workspaces/workspaces.c`).
//!
//! Each monitor shows its own 5-wide range of workspace ids: the monitor at
//! output index 0 shows 1-5, index 1 shows 6-10, and so on (the reference
//! bar's convention; the index is binding order, see `wayland/output.rs`).
//! Within that range, every occupied workspace gets a pill with its kanji
//! numeral, and the focused one too even if empty. Occupied pills are filled
//! pink with dark text; the focused one is a pink outline with pink text.
//!
//! The data comes from Hyprland (`j/workspaces`: `windows > 0` = occupied;
//! `j/activeworkspace`: the focused id). It's re-queried whenever a relevant
//! event arrives, not tracked incrementally; re-querying is cheap at the rate
//! these events come.

use crate::{
    config::{BAR, COLORS, Color, FONT, glyphs},
    hyprland,
    render::{Action, BoxRect, ClickTarget, module_box::draw_rect, text::TextCache},
};

/// Workspaces per monitor.
const PILLS_PER_OUTPUT: i32 = 5;
/// A pill's own border width. Deliberately thicker than a module box's 2px
/// (one-off tuning matching the reference bar, so it stays local).
const PILL_BORDER: f64 = 4.0;
/// How much wider than its numeral a pill is.
const PILL_PAD_X: f64 = 14.0;
/// How much taller than its numeral a pill is.
const PILL_PAD_Y: f64 = 6.0;

/// What Hyprland last said about workspaces.
#[derive(Debug, Default)]
pub(crate) struct Workspaces {
    /// Ids of workspaces with at least one window, sorted.
    occupied: Vec<i32>,
    /// The focused workspace id.
    focused: Option<i32>,
}

impl Workspaces {
    /// Re-queries Hyprland. Returns true if anything shown changed (the
    /// occupied set or the focused id); most window events change neither,
    /// and then nothing is redrawn.
    pub(crate) fn refresh(&mut self) -> bool {
        let mut occupied: Vec<i32> = hyprland::request_json("workspaces")
            .and_then(|json| {
                json.as_array().map(|list| {
                    list.iter()
                        .filter(|ws| {
                            ws.get("windows")
                                .and_then(serde_json::Value::as_i64)
                                .unwrap_or(0)
                                > 0
                        })
                        .filter_map(|ws| ws.get("id").and_then(serde_json::Value::as_i64))
                        .filter_map(|id| i32::try_from(id).ok())
                        .collect()
                })
            })
            .unwrap_or_default();
        occupied.sort_unstable();
        let focused = hyprland::request_json("activeworkspace")
            .and_then(|json| json.get("id").and_then(serde_json::Value::as_i64))
            .and_then(|id| i32::try_from(id).ok());

        let changed = occupied != self.occupied || focused != self.focused;
        self.occupied = occupied;
        self.focused = focused;
        changed
    }

    /// Draws this monitor's pills from `x` rightwards, vertically centered in
    /// `top..top + height`. Returns where the next module starts (one
    /// `item_spacing` past the last pill, or `x` if there are none) and each
    /// pill as a click target.
    pub(crate) fn render(
        &self,
        cr: &cairo::Context,
        text: &mut TextCache,
        x: f64,
        output_index: usize,
        (top, height): (f64, f64),
    ) -> Result<(f64, Vec<ClickTarget>), cairo::Error> {
        let index = i32::try_from(output_index).unwrap_or(0);
        let first = index * PILLS_PER_OUTPUT + 1;
        let mut cursor = x;
        let mut targets = Vec::new();
        for id in first..first + PILLS_PER_OUTPUT {
            let focused = self.focused == Some(id);
            if !focused && self.occupied.binary_search(&id).is_err() {
                continue;
            }
            let label = glyphs::kanji(id).map_or_else(|| id.to_string(), String::from);
            let (text_w, text_h) = text.size(&label, FONT.sm, true);
            let (w, h) = (text_w + PILL_PAD_X, text_h + PILL_PAD_Y);
            let pill = BoxRect {
                x: cursor,
                y: top + (height - h) / 2.0,
                w,
                h,
            };
            let (fill, ink) = if focused {
                (Color::TRANSPARENT, COLORS.pink)
            } else {
                (COLORS.pink, COLORS.base)
            };
            draw_rect(cr, pill, fill, COLORS.pink, PILL_BORDER)?;
            text.draw_centered(cr, pill, &label, FONT.sm, true, ink)?;
            targets.push(ClickTarget {
                rect: pill,
                action: Action::FocusWorkspace(id),
            });
            cursor += w + BAR.item_spacing;
        }
        Ok((cursor, targets))
    }
}
