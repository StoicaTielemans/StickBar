//! The memory dropdown (the C bar's `modules/mem/mem_popup.c`; what each
//! figure means: `docs/modules/sysmon.md`, "Memory dropdown").
//!
//! Top to bottom:
//! - header: total, swap kind, and a pill from memory *pressure* (PSI) rather
//!   than percent used (RAM that looks full but is mostly cache isn't a
//!   problem; stalling is);
//! - a graph card: used % (4 cells tall) and swap % (2 cells), with peaks;
//! - the time axis;
//! - the apps / cache / free split bar with its legend;
//! - rows: RAM and SWAP meters, ZRAM (only on zram), AVAIL, PRESS (only with PSI);
//! - TOP APPS by private memory (always 5 rows tall, so the open-time size
//!   never clips).
//!
//! Each section is drawn by a function taking `Option<&cairo::Context>`:
//! with `None` it only reports its height, which is how the dropdown's
//! height is measured before it opens (the same as C's `paint(NULL)` pass).

use std::fs::File;

use smithay_client_toolkit::reexports::client::protocol::wl_output::WlOutput;

use crate::{
    bar::{
        popup_host::PopupHost,
        popup_surface::{PopupEnv, PopupSurface, render_image},
    },
    config::{COLORS, Color, FONT, glyphs::ICON_MEM},
    render::{
        BoxRect, braille_graph, meter_bar,
        module_box::draw_rect,
        popup_chrome::{self, Header},
        popup_style::{COL_GAP, HEADER_H, OUTER_MARGIN, SECTION_GAP},
        stat_card::{
            self, CARD_PAD, GRAPH_LABEL_GAP, GRAPH_LABEL_H, GRAPH_MIN_CELLS_W, GRAPH_SPLIT_GAP,
            ROW_H,
        },
        text::TextCache,
        whole_px,
    },
    sysmon::{
        Sysmon,
        memory::{MAX_PROCS, Memory},
    },
    util::history::HISTORY_LEN,
};

/// The used % graph's height in cells.
const USED_CELLS_H: u32 = 4;
/// The swap % graph's height in cells.
const SWAP_CELLS_H: u32 = 2;
/// The apps/cache/free bar's height.
const SPLIT_BAR_H: f64 = 8.0;
/// Its segments.
const SPLIT_SEGMENTS: u32 = 48;
/// A legend colour square.
const LEGEND_SWATCH: f64 = 7.0;
/// Between legend entries.
const LEGEND_GAP: f64 = 14.0;
/// A RAM/SWAP meter's size and segments.
const METER_W: f64 = 110.0;
/// A RAM/SWAP meter's height.
const METER_H: f64 = 6.0;
/// A RAM/SWAP meter's segments.
const METER_SEGMENTS: u32 = 12;
/// The rows' labels (for the label column's width).
const ROW_LABELS: [&str; 5] = ["RAM", "SWAP", "ZRAM", "AVAIL", "PRESS"];
/// The fill gradient: yellow → peach → red.
const FILL: [Color; 3] = [COLORS.yellow, COLORS.peach, COLORS.red];
/// The swap graph's gradient.
const SWAP_FILL: [Color; 3] = [COLORS.peach, COLORS.maroon, COLORS.red];

/// The memory dropdown: closed (`host` is `None`) or open.
#[derive(Default)]
pub(crate) struct MemPopup {
    /// The open popup's surfaces.
    host: Option<PopupHost>,
    /// The popup's size, fixed while open.
    size: (f64, f64),
}

impl MemPopup {
    /// Opens the dropdown under the box `trigger` on `output` (re-opening it if
    /// it's already open, as in C): fresh details, then size, then paint.
    pub(crate) fn open<'o>(
        &mut self,
        env: &PopupEnv<'_>,
        output: &WlOutput,
        trigger: BoxRect,
        all_outputs: impl Iterator<Item = &'o WlOutput>,
        sysmon: &mut Sysmon,
        text: &mut TextCache,
    ) {
        self.host = None;
        sysmon.mem.refresh_details();
        let content_w = content_width(text, &sysmon.mem);
        let height = paint_content(None, text, sysmon, content_w).unwrap_or(0.0);
        self.size = (content_w + OUTER_MARGIN * 2.0, height);
        let size = (whole_px(self.size.0), whole_px(self.size.1));
        self.host = Some(PopupHost::open_left(
            env,
            output,
            trigger,
            size,
            all_outputs,
        ));
        self.paint(sysmon, text);
    }

    /// The 5 s poll ran: refresh the details and repaint, if open.
    pub(crate) fn refresh_if_open(&mut self, sysmon: &mut Sysmon, text: &mut TextCache) {
        if self.host.is_some() {
            sysmon.mem.refresh_details();
            self.paint(sysmon, text);
        }
    }

    /// Closes the dropdown (no-op if closed).
    pub(crate) fn close(&mut self) {
        self.host = None;
    }

    /// The open popup's surfaces, if open.
    pub(crate) const fn host(&self) -> Option<&PopupHost> {
        self.host.as_ref()
    }

    /// The open popup's surfaces, for configure routing.
    pub(crate) const fn host_mut(&mut self) -> Option<&mut PopupHost> {
        self.host.as_mut()
    }

    /// The menu surface, if open.
    pub(crate) fn menu(&self) -> Option<&PopupSurface> {
        self.host.as_ref().map(PopupHost::menu)
    }

    /// Paints the open popup.
    pub(crate) fn paint(&mut self, sysmon: &Sysmon, text: &mut TextCache) {
        let size = self.size;
        if let Some(host) = self.host.as_mut() {
            host.menu_mut().paint(|cr| draw(cr, text, sysmon, size));
        }
    }

    /// Writes what the open popup shows, drawn again, to a PNG (the tests' `snapshot`).
    pub(crate) fn snapshot(
        &self,
        path: &str,
        sysmon: &Sysmon,
        text: &mut TextCache,
    ) -> Result<(), String> {
        let host = self.host.as_ref().ok_or("no open menu to snapshot")?;
        let (width, height) = host.menu().menu_size().ok_or("not a menu")?;
        let size = self.size;
        let image = render_image(width, height, |cr| draw(cr, text, sysmon, size))
            .map_err(|e| e.to_string())?;
        let mut file = File::create(path).map_err(|e| e.to_string())?;
        image.write_to_png(&mut file).map_err(|e| e.to_string())
    }
}

/// The whole popup: frame, then the content.
fn draw(
    cr: &cairo::Context,
    text: &mut TextCache,
    sysmon: &Sysmon,
    (w, h): (f64, f64),
) -> Result<(), cairo::Error> {
    popup_chrome::draw_frame(cr, w, h, COLORS.yellow)?;
    paint_content(Some(cr), text, sysmon, w - OUTER_MARGIN * 2.0).map(|_height| ())
}

/// Every section, top to bottom, in a content column `w` wide. Returns the
/// popup's total height (also when only measuring, `cr` = `None`).
fn paint_content(
    cr: Option<&cairo::Context>,
    text: &mut TextCache,
    sysmon: &Sysmon,
    w: f64,
) -> Result<f64, cairo::Error> {
    let mem = &sysmon.mem;
    let x = OUTER_MARGIN;
    let mut y = OUTER_MARGIN;
    if let Some(cr) = cr {
        paint_header(cr, text, mem, (x, y, w))?;
    }
    y += HEADER_H + SECTION_GAP;
    y += paint_graph_card(cr, text, sysmon, (x, y, w))?;
    if let Some(cr) = cr {
        stat_card::draw_axis(cr, text, (x, y, w), stat_card::graph_cells(w, HISTORY_LEN))?;
    }
    y += stat_card::axis_height() + SECTION_GAP;
    y += paint_split(cr, text, mem, (x, y, w))?;
    y += SECTION_GAP;
    y += paint_rows(cr, text, mem, (x, y, w))?;
    y += SECTION_GAP;
    y += paint_apps(cr, text, mem, (x, y, w))?;
    Ok(y + OUTER_MARGIN)
}

/// "{GB} or {MB}": GiB with one decimal from 1 GiB up, whole MiB below.
fn format_gb(gb: f64) -> String {
    if gb >= 1.0 {
        format!("{gb:.1} GB")
    } else {
        format!("{:.0} MB", gb * 1024.0)
    }
}

/// The header's summary: total and swap kind.
fn header_summary(mem: &Memory) -> String {
    let swap = if mem.swap_total_gb <= 0.0 {
        "no swap"
    } else if mem.zram.is_some() {
        "zram swap"
    } else {
        "swap"
    };
    format!("  \u{b7}  {:.1} GB  \u{b7}  {swap}", mem.total_gb)
}

/// The pill: from PSI's 10 s average if the kernel has it, else from used %.
fn pill(mem: &Memory) -> (&'static str, Color) {
    match mem.psi {
        Some([some, ..]) if some < 1.0 => ("ok", COLORS.green),
        Some([some, ..]) if some < 10.0 => ("pressure", COLORS.yellow),
        Some(_) => ("thrashing", COLORS.red),
        None if mem.percent < 70 => ("ok", COLORS.green),
        None if mem.percent < 90 => ("high", COLORS.yellow),
        None => ("full", COLORS.red),
    }
}

/// The header row.
fn paint_header(
    cr: &cairo::Context,
    text: &mut TextCache,
    mem: &Memory,
    origin: (f64, f64, f64),
) -> Result<(), cairo::Error> {
    let summary = header_summary(mem);
    let (word, fill) = pill(mem);
    let header = Header {
        icon: Some(ICON_MEM),
        icon_color: COLORS.yellow,
        title: "Memory",
        summary: &summary,
        pill: Some(word),
        pill_fill: fill,
    };
    popup_chrome::draw_header(cr, text, origin, &header)
}

/// The used/swap graph card. Returns its height.
fn paint_graph_card(
    cr: Option<&cairo::Context>,
    text: &mut TextCache,
    sysmon: &Sysmon,
    (x, y, w): (f64, f64, f64),
) -> Result<f64, cairo::Error> {
    let cells = stat_card::graph_cells(w, HISTORY_LEN);
    let (used_h, swap_h) = (
        braille_graph::height(USED_CELLS_H),
        braille_graph::height(SWAP_CELLS_H),
    );
    let h = CARD_PAD * 2.0
        + (GRAPH_LABEL_H + GRAPH_LABEL_GAP) * 2.0
        + used_h
        + GRAPH_SPLIT_GAP
        + swap_h;
    let Some(cr) = cr else {
        return Ok(h);
    };
    popup_chrome::draw_card(cr, BoxRect { x, y, w, h }, false, false, COLORS.surface1)?;

    let mem = &sysmon.mem;
    let history = &sysmon.mem_history;
    let (first, count) = stat_card::history_window(history.len(), cells);
    let used: Vec<f64> = (first..first + count).map(|i| history.a(i)).collect();
    let swap: Vec<f64> = (first..first + count).map(|i| history.b(i)).collect();
    // Peaks start from the current figures, so "used 40%" never sits beside
    // "peak 31%" when the fresh read is ahead of the last poll's sample.
    let used_peak = used.iter().copied().fold(f64::from(mem.percent), f64::max);
    let swap_peak = swap
        .iter()
        .copied()
        .fold(f64::from(mem.swap_percent), f64::max);

    let (inner_x, inner_w) = (x + CARD_PAD, w - CARD_PAD * 2.0);
    let graph_x = x + (w - braille_graph::width(cells)) / 2.0;
    let mut cy = y + CARD_PAD;
    let used_label = format!("used {}%", mem.percent);
    let used_peak_label = format!("peak {used_peak:.0}%");
    stat_card::graph_label_row(
        cr,
        text,
        (inner_x, cy, inner_w),
        &used_label,
        COLORS.yellow,
        &used_peak_label,
    )?;
    cy += GRAPH_LABEL_H + GRAPH_LABEL_GAP;
    braille_graph::draw(
        cr,
        (graph_x, cy),
        (cells, USED_CELLS_H),
        &used,
        100.0,
        true,
        &FILL,
    )?;
    cy += used_h + GRAPH_SPLIT_GAP;

    let swap_label = format!("swap {}%", mem.swap_percent);
    let swap_peak_label = format!("peak {swap_peak:.0}%");
    stat_card::graph_label_row(
        cr,
        text,
        (inner_x, cy, inner_w),
        &swap_label,
        COLORS.yellow,
        &swap_peak_label,
    )?;
    cy += GRAPH_LABEL_H + GRAPH_LABEL_GAP;
    braille_graph::draw(
        cr,
        (graph_x, cy),
        (cells, SWAP_CELLS_H),
        &swap,
        100.0,
        true,
        &SWAP_FILL,
    )?;
    Ok(h)
}

/// The apps / cache / free bar and its legend. Returns its height.
fn paint_split(
    cr: Option<&cairo::Context>,
    text: &mut TextCache,
    mem: &Memory,
    (x, y, w): (f64, f64, f64),
) -> Result<f64, cairo::Error> {
    let h = SPLIT_BAR_H + 4.0 + ROW_H;
    let Some(cr) = cr else {
        return Ok(h);
    };
    let total = mem.total_gb;
    let (apps, cache) = (mem.used_gb, mem.cache_gb());
    let apps_end = if total > 0.0 { apps / total } else { 0.0 };
    let cache_end = if total > 0.0 {
        (apps + cache) / total
    } else {
        0.0
    };
    let count = f64::from(SPLIT_SEGMENTS);
    let seg_w = (w - meter_bar::SEGMENT_GAP * (count - 1.0)) / count;
    cr.save()?;
    for i in 0..SPLIT_SEGMENTS {
        // Each segment takes the colour of whatever its centre falls in.
        let centre = (f64::from(i) + 0.5) / count;
        let color = if centre < apps_end {
            COLORS.yellow
        } else if centre < cache_end {
            COLORS.blue
        } else {
            COLORS.surface1
        };
        color.set_source(cr);
        cr.rectangle(
            x + f64::from(i) * (seg_w + meter_bar::SEGMENT_GAP),
            y,
            seg_w,
            SPLIT_BAR_H,
        );
        cr.fill()?;
    }
    cr.restore()?;

    let ly = y + SPLIT_BAR_H + 4.0;
    let mut lx = x;
    let legend = [
        ("apps", apps, COLORS.yellow),
        ("cache", cache, COLORS.blue),
        ("free", mem.free_gb, COLORS.surface1),
    ];
    for (name, value, color) in legend {
        let swatch = BoxRect {
            x: lx,
            y: ly + (ROW_H - LEGEND_SWATCH) / 2.0,
            w: LEGEND_SWATCH,
            h: LEGEND_SWATCH,
        };
        draw_rect(cr, swatch, color, COLORS.mantle, 0.0)?;
        lx += LEGEND_SWATCH + 5.0;
        text.draw_left(cr, (lx, ly, ROW_H), name, FONT.xs, true, COLORS.overlay0)?;
        lx += text.width(name, FONT.xs, true) + 5.0;
        let value = format_gb(value);
        text.draw_left(cr, (lx, ly, ROW_H), &value, FONT.label, false, COLORS.text)?;
        lx += text.width(&value, FONT.label, false) + LEGEND_GAP;
    }
    Ok(h)
}

/// The ZRAM row's text: algorithm, stored → compressed (ratio), real use.
fn zram_text(mem: &Memory) -> String {
    let Some(z) = &mem.zram else {
        return String::new();
    };
    let ratio = if z.compr_mb > 0.0 {
        format!(" ({:.1}x)", z.orig_mb / z.compr_mb)
    } else {
        String::new()
    };
    let algorithm = if z.algorithm.is_empty() {
        "zram"
    } else {
        &z.algorithm
    };
    format!(
        "{algorithm} \u{b7} {} -> {}{ratio} \u{b7} uses {}",
        format_gb(z.orig_mb / 1024.0),
        format_gb(z.compr_mb / 1024.0),
        format_gb(z.used_mb / 1024.0)
    )
}

/// The AVAIL row's text.
fn avail_text(mem: &Memory) -> String {
    format!(
        "{} \u{b7} shared {} \u{b7} dirty {:.0} MB",
        format_gb(mem.avail_gb),
        format_gb(mem.shared_gb),
        mem.dirty_mb
    )
}

/// The PRESS row's text.
fn psi_text([s10, s60, s300]: [f64; 3]) -> String {
    format!("{s10:.2} / {s60:.2} / {s300:.2} %  (10s / 60s / 5m)")
}

/// A label + meter + "used / total" + percent row.
fn meter_row(
    cr: &cairo::Context,
    text: &mut TextCache,
    (x, y, w): (f64, f64, f64),
    label: &str,
    (used_gb, total_gb, percent): (f64, f64, i32),
) -> Result<(), cairo::Error> {
    text.draw_left(cr, (x, y, ROW_H), label, FONT.xs, true, COLORS.overlay0)?;
    let mx = x + stat_card::label_col_w(text, &ROW_LABELS) + COL_GAP;
    let meter = BoxRect {
        x: mx,
        y: y + (ROW_H - METER_H) / 2.0,
        w: METER_W,
        h: METER_H,
    };
    let fraction = if total_gb > 0.0 {
        used_gb / total_gb
    } else {
        0.0
    };
    meter_bar::draw(cr, meter, fraction, METER_SEGMENTS, &FILL, COLORS.surface0)?;
    let value = format!("{} / {total_gb:.1} GB", format_gb(used_gb));
    text.draw_left(
        cr,
        (mx + METER_W + COL_GAP, y, ROW_H),
        &value,
        FONT.label,
        false,
        COLORS.text,
    )?;
    text.draw_right(
        cr,
        (x + w, y, ROW_H),
        &format!("{percent}%"),
        FONT.label,
        false,
        COLORS.subtext0,
    )
}

/// A label + text row.
fn text_row(
    cr: &cairo::Context,
    text: &mut TextCache,
    (x, y): (f64, f64),
    label: &str,
    value: &str,
) -> Result<(), cairo::Error> {
    text.draw_left(cr, (x, y, ROW_H), label, FONT.xs, true, COLORS.overlay0)?;
    let value_x = x + stat_card::label_col_w(text, &ROW_LABELS) + COL_GAP;
    text.draw_left(
        cr,
        (value_x, y, ROW_H),
        value,
        FONT.label,
        false,
        COLORS.text,
    )
}

/// RAM, SWAP, then ZRAM / AVAIL / PRESS as available. Returns the height.
fn paint_rows(
    cr: Option<&cairo::Context>,
    text: &mut TextCache,
    mem: &Memory,
    (x, y, w): (f64, f64, f64),
) -> Result<f64, cairo::Error> {
    let text_rows = 1.0
        + if mem.zram.is_some() { 1.0 } else { 0.0 }
        + if mem.psi.is_some() { 1.0 } else { 0.0 };
    let h = ROW_H * (2.0 + text_rows);
    let Some(cr) = cr else {
        return Ok(h);
    };
    meter_row(
        cr,
        text,
        (x, y, w),
        "RAM",
        (mem.used_gb, mem.total_gb, mem.percent),
    )?;
    if mem.swap_total_gb > 0.0 {
        let swap = (mem.swap_used_gb, mem.swap_total_gb, mem.swap_percent);
        meter_row(cr, text, (x, y + ROW_H, w), "SWAP", swap)?;
    } else {
        text.draw_left(
            cr,
            (x, y + ROW_H, ROW_H),
            "SWAP",
            FONT.xs,
            true,
            COLORS.overlay0,
        )?;
        let value_x = x + stat_card::label_col_w(text, &ROW_LABELS) + COL_GAP;
        text.draw_left(
            cr,
            (value_x, y + ROW_H, ROW_H),
            "none",
            FONT.label,
            false,
            COLORS.subtext0,
        )?;
    }
    let mut ry = y + ROW_H * 2.0;
    if mem.zram.is_some() {
        text_row(cr, text, (x, ry), "ZRAM", &zram_text(mem))?;
        ry += ROW_H;
    }
    text_row(cr, text, (x, ry), "AVAIL", &avail_text(mem))?;
    ry += ROW_H;
    if let Some(psi) = mem.psi {
        text_row(cr, text, (x, ry), "PRESS", &psi_text(psi))?;
    }
    Ok(h)
}

/// TOP APPS: always `MAX_PROCS` rows tall. Returns the height.
fn paint_apps(
    cr: Option<&cairo::Context>,
    text: &mut TextCache,
    mem: &Memory,
    (x, y, w): (f64, f64, f64),
) -> Result<f64, cairo::Error> {
    let rows = f64::from(u32::try_from(MAX_PROCS).unwrap_or(0));
    let h = ROW_H * (1.0 + rows);
    let Some(cr) = cr else {
        return Ok(h);
    };
    text.draw_left(
        cr,
        (x, y, ROW_H),
        "TOP APPS",
        FONT.xs,
        true,
        COLORS.overlay0,
    )?;
    let biggest = mem.procs.first().map_or(0.0, |(_, gb)| *gb);
    let mut ry = y;
    for (name, gb) in &mem.procs {
        ry += ROW_H;
        let fraction = if biggest > 0.0 { gb / biggest } else { 0.0 };
        stat_card::draw_app_row(
            cr,
            text,
            (x, ry, w),
            name,
            fraction,
            COLORS.yellow,
            &format!("{gb:.2} GB"),
        )?;
    }
    Ok(h)
}

/// The content column's width: the widest of the graph's minimum, the header,
/// and the widest row any section can have (worst-case sample strings, so the
/// size fixed at open never clips a later value).
fn content_width(text: &mut TextCache, mem: &Memory) -> f64 {
    let graph_w = braille_graph::width(GRAPH_MIN_CELLS_W) + CARD_PAD * 2.0;
    let summary = header_summary(mem);
    let header = Header {
        icon: Some(ICON_MEM),
        icon_color: COLORS.yellow,
        title: "Memory",
        summary: &summary,
        pill: Some("thrashing"),
        pill_fill: COLORS.red,
    };
    let header_w = popup_chrome::header_width(text, &header);
    let label_w = stat_card::label_col_w(text, &ROW_LABELS);
    let zram = zram_text(mem);
    let samples = [
        "00.0 GB \u{b7} shared 000 MB \u{b7} dirty 0000 MB",
        "00.00 / 00.00 / 00.00 %  (10s / 60s / 5m)",
        &zram,
        "zstd \u{b7} 000 MB -> 000 MB (0.0x) \u{b7} uses 000 MB",
    ];
    let rows_w = samples
        .iter()
        .map(|value| label_w + COL_GAP + text.width(value, FONT.label, false))
        .fold(0.0, f64::max);
    let meter_w = label_w
        + COL_GAP
        + METER_W
        + COL_GAP
        + text.width("000 MB / 00.0 GB", FONT.label, false)
        + COL_GAP
        + text.width("100%", FONT.label, false);
    let apps_w = stat_card::app_name_col_w(text)
        + stat_card::APP_BAR_W
        + COL_GAP
        + text.width("00.00 GB", FONT.label, false);
    [graph_w, header_w, rows_w, meter_w, apps_w]
        .into_iter()
        .fold(0.0, f64::max)
}
