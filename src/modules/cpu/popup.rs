//! The CPU dropdown (the C bar's `modules/cpu/cpu_popup.c`; what each figure
//! means: `docs/modules/sysmon.md`, "CPU dropdown"). The memory dropdown's
//! twin in layout.
//!
//! Top to bottom:
//! - header: short model, cores/threads, and a load pill (idle <15 % green,
//!   active <60 % yellow, busy peach);
//! - a graph card: usage % (4 cells) and temperature (2 cells, 30-90 °C);
//! - the time axis;
//! - the per-thread grid: index, meter, %, current clock, column-major in two
//!   columns like btop, split into P-CORES / E-CORES on hybrid Intel chips;
//! - rows: LOAD, CLOCK, UPTIME;
//! - TOP PROCESSES by CPU %.
//!
//! **First figures within a second:** opening resets the process sample, so
//! the open-time scan only primes it ("measuring..."). `tick()`, on the 1 s
//! clock timer, does the second scan at the first tick at least 0.5 s after
//! opening (a tick can come milliseconds after open, which would measure
//! nothing). After that the 5 s poll keeps it fresh.
//!
//! Sections take `Option<&cairo::Context>`: `None` only measures (the open-time
//! height pass), as in the memory dropdown.

use std::{
    fs::File,
    time::{Duration, Instant},
};

use smithay_client_toolkit::reexports::client::protocol::wl_output::WlOutput;

use crate::{
    bar::{
        popup_host::PopupHost,
        popup_surface::{PopupEnv, PopupSurface, render_image},
    },
    config::{COLORS, Color, FONT, glyphs::ICON_CPU},
    render::{
        BoxRect, braille_graph, meter_bar,
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
        cpu::{Cpu, MAX_PROCS, short_model},
    },
    util::history::HISTORY_LEN,
};

/// The usage graph's height in cells.
const USAGE_CELLS_H: u32 = 4;
/// The temperature graph's height in cells.
const TEMP_CELLS_H: u32 = 2;
/// One row of the per-thread grid.
const CORE_ROW_H: f64 = 15.0;
/// Between the parts of one thread's cell.
const CELL_GAP: f64 = 6.0;
/// Between the grid's two columns.
const CORE_COLUMN_GAP: f64 = 18.0;
/// A thread's meter.
const CORE_METER_W: f64 = 60.0;
/// A thread's meter height.
const CORE_METER_H: f64 = 6.0;
/// A thread's meter segments.
const CORE_METER_SEGMENTS: u32 = 6;
/// The temperature graph's floor: 30 °C reads as empty.
const TEMP_GRAPH_MIN_C: f64 = 30.0;
/// The temperature graph's ceiling: 90 °C reads as full.
const TEMP_GRAPH_MAX_C: f64 = 90.0;
/// The first-figures tick waits at least this long after opening.
const MIN_FIRST_SAMPLE: Duration = Duration::from_millis(500);
/// The rows' labels (for the label column's width).
const ROW_LABELS: [&str; 3] = ["LOAD", "CLOCK", "UPTIME"];
/// Usage colouring: green → yellow → red.
const HEAT: [Color; 3] = [COLORS.green, COLORS.yellow, COLORS.red];
/// Temperature colouring.
const TEMP_FILL: [Color; 3] = [COLORS.peach, COLORS.maroon, COLORS.red];

/// The CPU dropdown: closed (`host` is `None`) or open.
#[derive(Default)]
pub(crate) struct CpuPopup {
    /// The open popup's surfaces.
    host: Option<PopupHost>,
    /// The popup's size, fixed while open.
    size: (f64, f64),
    /// When it opened, until the first-figures tick has run (then `None`).
    awaiting_first_tick: Option<Instant>,
}

impl CpuPopup {
    /// Opens the dropdown under the box `trigger` on `output` (re-opening it
    /// if it's open, as in C): a fresh process window, details, size, paint.
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
        sysmon.cpu.reset_process_sample();
        sysmon.cpu.refresh_details();
        self.awaiting_first_tick = Some(Instant::now());
        let content_w = content_width(text, &sysmon.cpu);
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

    /// The 1 s clock tick: the second process scan, once, at least 0.5 s
    /// after opening (see the module docs).
    pub(crate) fn tick(&mut self, sysmon: &mut Sysmon, text: &mut TextCache) {
        let due = self
            .awaiting_first_tick
            .is_some_and(|opened| opened.elapsed() >= MIN_FIRST_SAMPLE);
        if self.host.is_some() && due {
            self.awaiting_first_tick = None;
            sysmon.cpu.refresh_details();
            self.paint(sysmon, text);
        }
    }

    /// The 5 s poll ran: refresh the details and repaint, if open.
    pub(crate) fn refresh_if_open(&mut self, sysmon: &mut Sysmon, text: &mut TextCache) {
        if self.host.is_some() {
            sysmon.cpu.refresh_details();
            self.paint(sysmon, text);
        }
    }

    /// Closes the dropdown (no-op if closed).
    pub(crate) fn close(&mut self) {
        self.host = None;
        self.awaiting_first_tick = None;
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
    popup_chrome::draw_frame(cr, w, h, COLORS.peach)?;
    paint_content(Some(cr), text, sysmon, w - OUTER_MARGIN * 2.0).map(|_height| ())
}

/// Every section, top to bottom, in a content column `w` wide. Returns the
/// popup's total height (also when only measuring).
fn paint_content(
    cr: Option<&cairo::Context>,
    text: &mut TextCache,
    sysmon: &Sysmon,
    w: f64,
) -> Result<f64, cairo::Error> {
    let cpu = &sysmon.cpu;
    let x = OUTER_MARGIN;
    let mut y = OUTER_MARGIN;
    if let Some(cr) = cr {
        paint_header(cr, text, cpu, (x, y, w))?;
    }
    y += HEADER_H + SECTION_GAP;
    y += paint_graph_card(cr, text, sysmon, (x, y, w))?;
    if let Some(cr) = cr {
        stat_card::draw_axis(cr, text, (x, y, w), stat_card::graph_cells(w, HISTORY_LEN))?;
    }
    y += stat_card::axis_height() + SECTION_GAP;
    y += paint_cores(cr, text, cpu, (x, y))?;
    y += SECTION_GAP;
    y += paint_rows(cr, text, cpu, (x, y))?;
    y += SECTION_GAP;
    y += paint_apps(cr, text, cpu, (x, y, w))?;
    Ok(y + OUTER_MARGIN)
}

/// The header's summary: cores and threads.
fn counts_text(cpu: &Cpu) -> String {
    if cpu.physical_cores > 0 {
        format!(
            "  \u{b7}  {} cores / {} threads",
            cpu.physical_cores,
            cpu.cores.len()
        )
    } else {
        format!("  \u{b7}  {} threads", cpu.cores.len())
    }
}

/// The header row.
fn paint_header(
    cr: &cairo::Context,
    text: &mut TextCache,
    cpu: &Cpu,
    origin: (f64, f64, f64),
) -> Result<(), cairo::Error> {
    let model = short_model(&cpu.model);
    let counts = counts_text(cpu);
    let (pill, fill) = if cpu.percent < 15.0 {
        ("idle", COLORS.green)
    } else if cpu.percent < 60.0 {
        ("active", COLORS.yellow)
    } else {
        ("busy", COLORS.peach)
    };
    let header = Header {
        icon: Some(ICON_CPU),
        icon_color: COLORS.peach,
        title: &model,
        summary: &counts,
        pill: Some(pill),
        pill_fill: fill,
    };
    popup_chrome::draw_header(cr, text, origin, &header)
}

/// The usage/temperature graph card. Returns its height.
fn paint_graph_card(
    cr: Option<&cairo::Context>,
    text: &mut TextCache,
    sysmon: &Sysmon,
    (x, y, w): (f64, f64, f64),
) -> Result<f64, cairo::Error> {
    let cells = stat_card::graph_cells(w, HISTORY_LEN);
    let (usage_h, temp_h) = (
        braille_graph::height(USAGE_CELLS_H),
        braille_graph::height(TEMP_CELLS_H),
    );
    let h = CARD_PAD * 2.0
        + (GRAPH_LABEL_H + GRAPH_LABEL_GAP) * 2.0
        + usage_h
        + GRAPH_SPLIT_GAP
        + temp_h;
    let Some(cr) = cr else {
        return Ok(h);
    };
    popup_chrome::draw_card(cr, BoxRect { x, y, w, h }, false, false, COLORS.surface1)?;

    let cpu = &sysmon.cpu;
    let history = &sysmon.cpu_history;
    let (first, count) = stat_card::history_window(history.len(), cells);
    let usage: Vec<f64> = (first..first + count).map(|i| history.a(i)).collect();
    let temps: Vec<f64> = (first..first + count).map(|i| history.b(i)).collect();
    // The graph shows degrees above the 30 °C floor.
    let temp_dots: Vec<f64> = temps
        .iter()
        .map(|&t| (t - TEMP_GRAPH_MIN_C).max(0.0))
        .collect();
    let usage_peak = usage.iter().copied().fold(0.0, f64::max);
    let temp_peak = temps.iter().copied().fold(0.0, f64::max);

    let (inner_x, inner_w) = (x + CARD_PAD, w - CARD_PAD * 2.0);
    let graph_x = x + (w - braille_graph::width(cells)) / 2.0;
    let mut cy = y + CARD_PAD;
    let usage_label = format!("usage {:.0}%", cpu.percent);
    let usage_peak_label = format!("peak {usage_peak:.0}%");
    stat_card::graph_label_row(
        cr,
        text,
        (inner_x, cy, inner_w),
        &usage_label,
        COLORS.peach,
        &usage_peak_label,
    )?;
    cy += GRAPH_LABEL_H + GRAPH_LABEL_GAP;
    braille_graph::draw(
        cr,
        (graph_x, cy),
        (cells, USAGE_CELLS_H),
        &usage,
        100.0,
        true,
        &HEAT,
    )?;
    cy += usage_h + GRAPH_SPLIT_GAP;

    let temp_label = format!("temp {}\u{b0}C", cpu.temp_c);
    let temp_peak_label = format!("max {temp_peak:.0}\u{b0}C");
    stat_card::graph_label_row(
        cr,
        text,
        (inner_x, cy, inner_w),
        &temp_label,
        COLORS.peach,
        &temp_peak_label,
    )?;
    cy += GRAPH_LABEL_H + GRAPH_LABEL_GAP;
    let range = TEMP_GRAPH_MAX_C - TEMP_GRAPH_MIN_C;
    braille_graph::draw(
        cr,
        (graph_x, cy),
        (cells, TEMP_CELLS_H),
        &temp_dots,
        range,
        true,
        &TEMP_FILL,
    )?;
    Ok(h)
}

/// The widths of a thread cell's parts: index, percent, clock.
fn cell_parts(text: &mut TextCache) -> (f64, f64, f64) {
    (
        text.width("00", FONT.sm, false),
        text.width("100%", FONT.sm, false),
        text.width("0.0G", FONT.xs, false),
    )
}

/// One thread cell's total width.
fn core_cell_w(text: &mut TextCache) -> f64 {
    let (index_w, percent_w, ghz_w) = cell_parts(text);
    index_w + CELL_GAP + CORE_METER_W + CELL_GAP + percent_w + CELL_GAP + ghz_w
}

/// One thread's cell at (x, y): number, meter, %, clock.
fn draw_core_cell(
    cr: &cairo::Context,
    text: &mut TextCache,
    cpu: &Cpu,
    (x, y): (f64, f64),
    core: usize,
) -> Result<(), cairo::Error> {
    let (index_w, percent_w, _) = cell_parts(text);
    text.draw_right(
        cr,
        (x + index_w, y, CORE_ROW_H),
        &core.to_string(),
        FONT.sm,
        false,
        COLORS.overlay0,
    )?;
    let mut cx = x + index_w + CELL_GAP;
    let percent = cpu.cores.get(core).copied().unwrap_or(0.0);
    let meter = BoxRect {
        x: cx,
        y: y + (CORE_ROW_H - CORE_METER_H) / 2.0,
        w: CORE_METER_W,
        h: CORE_METER_H,
    };
    meter_bar::draw(
        cr,
        meter,
        percent / 100.0,
        CORE_METER_SEGMENTS,
        &HEAT,
        COLORS.surface0,
    )?;
    cx += CORE_METER_W + CELL_GAP;
    text.draw_right(
        cr,
        (cx + percent_w, y, CORE_ROW_H),
        &format!("{percent:.0}%"),
        FONT.sm,
        false,
        COLORS.text,
    )?;
    cx += percent_w + CELL_GAP;
    let ghz = cpu.core_ghz.get(core).copied().unwrap_or(0.0);
    if ghz > 0.0 {
        text.draw_left(
            cr,
            (cx, y, CORE_ROW_H),
            &format!("{ghz:.1}G"),
            FONT.xs,
            false,
            COLORS.overlay0,
        )?;
    }
    Ok(())
}

/// One group of threads (all, or the P- or E-cores) under `title`, in two
/// columns filled top to bottom. Returns its height (0 if it has none).
fn paint_core_group(
    cr: Option<&cairo::Context>,
    text: &mut TextCache,
    cpu: &Cpu,
    (x, y): (f64, f64),
    (title, efficient): (&str, Option<bool>),
) -> Result<f64, cairo::Error> {
    let threads: Vec<usize> = (0..cpu.cores.len())
        .filter(|&i| efficient.is_none_or(|e| cpu.is_efficient(i) == e))
        .collect();
    if threads.is_empty() {
        return Ok(0.0);
    }
    let rows = threads.len().div_ceil(2);
    let h = ROW_H + f64::from(u32::try_from(rows).unwrap_or(0)) * CORE_ROW_H;
    let Some(cr) = cr else {
        return Ok(h);
    };
    text.draw_left(cr, (x, y, ROW_H), title, FONT.xs, true, COLORS.overlay0)?;
    let column_w = core_cell_w(text) + CORE_COLUMN_GAP;
    for (i, &core) in threads.iter().enumerate() {
        let (column, row) = (i / rows, i % rows);
        let cell_x = x + f64::from(u32::try_from(column).unwrap_or(0)) * column_w;
        let cell_y = y + ROW_H + f64::from(u32::try_from(row).unwrap_or(0)) * CORE_ROW_H;
        draw_core_cell(cr, text, cpu, (cell_x, cell_y), core)?;
    }
    Ok(h)
}

/// The per-thread grid: one CORES group, or P-CORES then E-CORES. Returns its height.
fn paint_cores(
    cr: Option<&cairo::Context>,
    text: &mut TextCache,
    cpu: &Cpu,
    (x, y): (f64, f64),
) -> Result<f64, cairo::Error> {
    if !cpu.hybrid {
        return paint_core_group(cr, text, cpu, (x, y), ("CORES", None));
    }
    let mut h = paint_core_group(cr, text, cpu, (x, y), ("P-CORES", Some(false)))?;
    h += 4.0;
    h += paint_core_group(cr, text, cpu, (x, y + h), ("E-CORES", Some(true)))?;
    Ok(h)
}

/// The LOAD, CLOCK and UPTIME rows' values.
fn row_texts(cpu: &Cpu) -> [String; 3] {
    let load = format!(
        "{} \u{b7} {} running / {} tasks",
        cpu.load, cpu.tasks_running, cpu.tasks_total
    );
    let clocks: Vec<f64> = cpu
        .core_ghz
        .iter()
        .copied()
        .filter(|&ghz| ghz > 0.0)
        .collect();
    let avg = if clocks.is_empty() {
        cpu.fallback_ghz
    } else {
        clocks.iter().sum::<f64>() / f64::from(u32::try_from(clocks.len()).unwrap_or(1))
    };
    let clock = if cpu.max_ghz > 0.0 {
        format!("avg {avg:.1} GHz \u{b7} max {:.1} GHz", cpu.max_ghz)
    } else {
        format!("avg {avg:.1} GHz")
    };
    let up = whole_px(cpu.uptime_s.floor());
    let (days, hours, minutes) = (up / 86_400, (up % 86_400) / 3600, (up % 3600) / 60);
    let uptime = if days > 0 {
        format!("{days}d {hours}h {minutes}m")
    } else {
        format!("{hours}h {minutes}m")
    };
    [load, clock, uptime]
}

/// LOAD, CLOCK, UPTIME. Returns the height.
fn paint_rows(
    cr: Option<&cairo::Context>,
    text: &mut TextCache,
    cpu: &Cpu,
    (x, y): (f64, f64),
) -> Result<f64, cairo::Error> {
    let h = ROW_H * 3.0;
    let Some(cr) = cr else {
        return Ok(h);
    };
    let value_x = x + stat_card::label_col_w(text, &ROW_LABELS) + COL_GAP;
    let mut ry = y;
    for (label, value) in ROW_LABELS.iter().zip(row_texts(cpu)) {
        stat_card::draw_kv_row(cr, text, (x, value_x, ry), label, &value)?;
        ry += ROW_H;
    }
    Ok(h)
}

/// TOP PROCESSES: always `MAX_PROCS` rows tall. Returns the height.
fn paint_apps(
    cr: Option<&cairo::Context>,
    text: &mut TextCache,
    cpu: &Cpu,
    (x, y, w): (f64, f64, f64),
) -> Result<f64, cairo::Error> {
    let h = ROW_H * (1.0 + f64::from(u32::try_from(MAX_PROCS).unwrap_or(0)));
    let Some(cr) = cr else {
        return Ok(h);
    };
    text.draw_left(
        cr,
        (x, y, ROW_H),
        "TOP PROCESSES",
        FONT.xs,
        true,
        COLORS.overlay0,
    )?;
    let note = if cpu.procs_ready {
        cpu.procs.is_empty().then_some("nothing busy")
    } else {
        Some("measuring...")
    };
    if let Some(note) = note {
        return text
            .draw_left(
                cr,
                (x, y + ROW_H, ROW_H),
                note,
                FONT.label,
                false,
                COLORS.subtext0,
            )
            .map(|()| h);
    }
    let biggest = cpu.procs.first().map_or(0.0, |(_, percent)| *percent);
    let mut ry = y;
    for (name, percent) in &cpu.procs {
        ry += ROW_H;
        let fraction = if biggest > 0.0 {
            percent / biggest
        } else {
            0.0
        };
        stat_card::draw_app_row(
            cr,
            text,
            (x, ry, w),
            name,
            fraction,
            COLORS.peach,
            &format!("{percent:.1}%"),
        )?;
    }
    Ok(h)
}

/// The content column's width: the widest of the graph's minimum, the
/// two-column grid, the header and the rows (with a worst-case load line).
fn content_width(text: &mut TextCache, cpu: &Cpu) -> f64 {
    let graph_w = braille_graph::width(GRAPH_MIN_CELLS_W) + CARD_PAD * 2.0;
    let cores_w = core_cell_w(text) * 2.0 + CORE_COLUMN_GAP;
    let model = short_model(&cpu.model);
    let counts = counts_text(cpu);
    let header = Header {
        icon: Some(ICON_CPU),
        icon_color: COLORS.peach,
        title: &model,
        summary: &counts,
        pill: Some("active"),
        pill_fill: COLORS.yellow,
    };
    let header_w = popup_chrome::header_width(text, &header);
    let label_w = stat_card::label_col_w(text, &ROW_LABELS);
    let [load, clock, _uptime] = row_texts(cpu);
    let samples = [
        load.as_str(),
        clock.as_str(),
        "00.00 00.00 00.00 \u{b7} 00 running / 00000 tasks",
    ];
    let rows_w = samples
        .iter()
        .map(|value| label_w + COL_GAP + text.width(value, FONT.label, false))
        .fold(0.0, f64::max);
    let apps_w = stat_card::app_name_col_w(text)
        + stat_card::APP_BAR_W
        + COL_GAP
        + text.width("100.0%", FONT.label, false);
    [graph_w, cores_w, header_w, rows_w, apps_w]
        .into_iter()
        .fold(0.0, f64::max)
}
