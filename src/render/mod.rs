//! Drawing one bar (the C bar's `render/render.c`).
//!
//! The bar has three regions, as in the C bar: left-region modules laid out
//! left to right from x = 0, right-region ones right to left from the right
//! edge, and the media box centered between. So far: the Arch logo, the
//! workspace pills, CPU, GPU and memory on the left, the clock on the right.

pub(crate) mod braille_graph;
pub(crate) mod meter_bar;
pub(crate) mod module_box;
pub(crate) mod popup_chrome;
pub(crate) mod popup_style;
pub(crate) mod stat_card;
pub(crate) mod text;

use crate::{
    config::{BAR, COLORS},
    modules::{clock::Clock, cpu, gpu, mem, powermenu::arch_logo, workspaces::Workspaces},
    sysmon::Sysmon,
};
use text::TextCache;

/// A rectangle in bar-surface pixels (cairo works in `f64`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct BoxRect {
    /// Left edge.
    pub(crate) x: f64,
    /// Top edge.
    pub(crate) y: f64,
    /// Width.
    pub(crate) w: f64,
    /// Height.
    pub(crate) h: f64,
}

impl BoxRect {
    /// Whether (x, y) is inside: left/top edges included, right/bottom excluded,
    /// so neighbouring boxes never both claim a point.
    pub(crate) fn contains(self, x: f64, y: f64) -> bool {
        x >= self.x && x < self.x + self.w && y >= self.y && y < self.y + self.h
    }
}

/// Converts a length or position in px from cairo's `f64` to a whole number,
/// dropping the fraction (like the C bar's `(int)` casts), for Wayland requests
/// that take integers (surface sizes, margins). Out-of-range values saturate.
#[expect(
    clippy::as_conversions,
    clippy::cast_possible_truncation,
    reason = "f64 -> i32 has no lossless conversion; `as` saturates and drops the fraction, which is the intent"
)]
pub(crate) fn whole_px(value: f64) -> i32 {
    value as i32
}

/// Where one module's box was drawn. Each bar keeps the list from its last full
/// redraw: the tests read it (`rects`), and from M4 clicks are hit-tested against it.
#[derive(Clone, Copy, Debug)]
pub(crate) struct ModuleBox {
    /// The module's name, as the tests know it (`clock`, `cpu`, ...).
    pub(crate) name: &'static str,
    /// Where its box is.
    pub(crate) rect: BoxRect,
    /// Whether the box reacts to a click (any button). The pointer turns into
    /// a hand over it (a Rust-only addition: see `docs/porting.md`).
    pub(crate) clickable: bool,
}

/// Something a click can do, found by hit-testing a [`ClickTarget`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Action {
    /// Focus Hyprland workspace `id` (a workspace pill, left button).
    FocusWorkspace(i32),
}

/// A clickable spot inside a module's box. A box like `workspaces` holds
/// several (one per pill), each doing something different; the gaps between
/// them do nothing. Tray icons will work the same way.
#[derive(Clone, Copy, Debug)]
pub(crate) struct ClickTarget {
    /// Where it is, in bar-surface px.
    pub(crate) rect: BoxRect,
    /// What a click there does.
    pub(crate) action: Action,
}

/// Where everything on one bar was drawn: the module boxes and the click
/// targets inside them.
#[derive(Debug, Default)]
pub(crate) struct BarLayout {
    /// One per module shown.
    pub(crate) boxes: Vec<ModuleBox>,
    /// Clickable spots within boxes.
    pub(crate) targets: Vec<ClickTarget>,
}

/// The drawing state every bar shares: the text cache and each module's data.
/// One instance, owned by `App`, used by every monitor's bar.
#[derive(Debug)]
pub(crate) struct Renderer {
    /// Shaped-text cache (pango). Popups draw with it too.
    pub(crate) text: TextCache,
    /// The clock module.
    pub(crate) clock: Clock,
    /// The workspace pills' data (from Hyprland).
    pub(crate) workspaces: Workspaces,
    /// CPU/GPU/memory figures (memory so far).
    pub(crate) sysmon: Sysmon,
}

impl Renderer {
    /// Sets up pango and the modules.
    pub(crate) fn new() -> Result<Self, cairo::Error> {
        Ok(Self {
            text: TextCache::new()?,
            clock: Clock::new(),
            workspaces: Workspaces::default(),
            sysmon: {
                // The first poll before anything is drawn, as the C bar does.
                let mut sysmon = Sysmon::new();
                sysmon.poll();
                sysmon
            },
        })
    }

    /// Draws the whole bar of the monitor at `output_index` (`width` x
    /// `height`) into `cr` and returns where everything landed.
    pub(crate) fn render_output(
        &mut self,
        cr: &cairo::Context,
        width: f64,
        height: f64,
        output_index: usize,
    ) -> Result<BarLayout, cairo::Error> {
        COLORS.mantle.set_source(cr);
        cr.paint()?;
        let mut layout = BarLayout::default();

        // Left region, laid out from x = 0 rightwards. Each module returns
        // where the next one starts (its right edge + item_spacing), so its
        // own box is everything up to there minus that gap (C's record_left).
        let arch = arch_logo::render(cr, &mut self.text, 0.0, (0.0, height))?;
        layout.boxes.push(ModuleBox {
            name: "arch",
            rect: arch,
            // Left click opens the power menu.
            clickable: true,
        });
        let mut left = arch.x + arch.w + BAR.item_spacing;
        let (next, pills) =
            self.workspaces
                .render(cr, &mut self.text, left, output_index, (0.0, height))?;
        let pills_w = next - BAR.item_spacing - left;
        if pills_w > 0.0 {
            layout.boxes.push(ModuleBox {
                name: "workspaces",
                rect: BoxRect {
                    x: left,
                    y: 0.0,
                    w: pills_w,
                    h: height,
                },
                // Only the pills themselves are clickable (as targets), not
                // the gaps between them.
                clickable: false,
            });
        }
        layout.targets.extend(pills);
        // With no pills, `next` is still `left` (no gap is added).
        left = next;

        let cpu = cpu::render(cr, &mut self.text, &self.sysmon.cpu, left, (0.0, height))?;
        layout.boxes.push(ModuleBox {
            name: "cpu",
            rect: cpu,
            // Right click opens its dropdown.
            clickable: true,
        });
        left = cpu.x + cpu.w + BAR.item_spacing;

        let gpu = gpu::render(cr, &mut self.text, &self.sysmon.gpu, left, (0.0, height))?;
        layout.boxes.push(ModuleBox {
            name: "gpu",
            rect: gpu,
            // Right click opens its dropdown.
            clickable: true,
        });
        left = gpu.x + gpu.w + BAR.item_spacing;

        let mem = mem::render(cr, &mut self.text, &self.sysmon.mem, left, (0.0, height))?;
        layout.boxes.push(ModuleBox {
            name: "mem",
            rect: mem,
            // Right click opens its dropdown.
            clickable: true,
        });

        // Right region, laid out from the right edge leftwards.
        let clock = self
            .clock
            .render(cr, &mut self.text, width, (0.0, height))?;
        layout.boxes.push(ModuleBox {
            name: "clock",
            rect: clock,
            // Right-click opens the year calendar.
            clickable: true,
        });
        Ok(layout)
    }

    /// Repaints only the clock, over its previous box `old`, on a frame that
    /// otherwise already holds the previous picture (C's
    /// `render_output_clock_only`). The clock ticks far more often than anything
    /// else changes, so this skips laying out every other module.
    ///
    /// Returns the new box, or `None` if its width changed. Then the boxes to
    /// its left would have to move, so the caller must do a full redraw instead.
    pub(crate) fn render_clock_only(
        &mut self,
        cr: &cairo::Context,
        old: BoxRect,
    ) -> Result<Option<BoxRect>, cairo::Error> {
        cr.save()?;
        cr.rectangle(old.x, old.y, old.w, old.h);
        cr.clip();
        COLORS.mantle.set_source(cr);
        cr.paint()?;
        let new = self
            .clock
            .render(cr, &mut self.text, old.x + old.w, (old.y, old.h))?;
        cr.restore()?;
        // Widths are whole pixels (pango pixel sizes plus fixed padding), so a
        // half-pixel tolerance is an exact comparison without float `==`.
        Ok(((new.w - old.w).abs() < 0.5).then_some(new))
    }
}
