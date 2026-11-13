//! The GPU dropdown (the C bar's `modules/gpu/gpu_popup.c`; what each figure
//! means: `docs/modules/sysmon.md`). The CPU and memory dropdowns' twin.
//!
//! Top to bottom:
//! - header: short name, driver version, and a P-state pill (P0-2 `busy`
//!   peach, P3-7 `active` yellow, P8+ `idle` green);
//! - a graph card: usage % (4 cells) and temperature (2 cells, 30-90 °C);
//! - the time axis;
//! - VRAM, POWER and FAN meters; CLOCK and VIDEO (encoder/decoder) lines;
//! - TOP VRAM APPS, always 5 rows tall.
//!
//! Without an NVIDIA GPU it says so explicitly instead ("No NVIDIA GPU
//! detected"), never showing zeros that look like real readings.
//!
//! Sections take `Option<&cairo::Context>`: `None` only measures (the
//! open-time height pass), as in the CPU and memory dropdowns.

use std::fs::File;

use smithay_client_toolkit::reexports::client::protocol::wl_output::WlOutput;

use crate::{
    bar::{
        popup_host::PopupHost,
        popup_surface::{PopupEnv, PopupSurface, render_image},
    },
    config::{COLORS, Color, FONT, glyphs::ICON_GPU},
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
        gpu::{Gpu, MAX_PROCS, short_name},
    },
    util::history::HISTORY_LEN,
};

/// The usage graph's height in cells.
const USAGE_CELLS_H: u32 = 4;
/// The temperature graph's height in cells.
const TEMP_CELLS_H: u32 = 2;
/// A VRAM/POWER/FAN meter.
const METER_W: f64 = 110.0;
/// A meter's height.
const METER_H: f64 = 7.0;
/// A meter's segments.
const METER_SEGMENTS: u32 = 10;
/// The temperature graph's floor: 30 °C reads as empty.
const TEMP_GRAPH_MIN_C: f64 = 30.0;
/// The temperature graph's ceiling: 90 °C reads as full.
const TEMP_GRAPH_MAX_C: f64 = 90.0;
/// The rows' labels (for the label column's width).
const ROW_LABELS: [&str; 5] = ["VRAM", "POWER", "FAN", "CLOCK", "VIDEO"];
/// Usage colouring: green → yellow → red.
const HEAT: [Color; 3] = [COLORS.green, COLORS.yellow, COLORS.red];
/// Temperature colouring.
const TEMP_FILL: [Color; 3] = [COLORS.peach, COLORS.maroon, COLORS.red];
/// The header's title without a GPU.
const NO_GPU: &str = "No NVIDIA GPU detected";

/// The GPU dropdown: closed (`host` is `None`) or open.
#[derive(Default)]
pub(crate) struct GpuPopup {
    /// The open popup's surfaces.
    host: Option<PopupHost>,
    /// The popup's size, fixed while open.
    size: (f64, f64),
}

impl GpuPopup {
    /// Opens the dropdown under the box `trigger` on `output` (re-opening it
    /// if it's open, as in C): fresh details, size, paint.
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
        sysmon.gpu.refresh_details();
        let content_w = content_width(text, &sysmon.gpu);
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
            sysmon.gpu.refresh_details();
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
    popup_chrome::draw_frame(cr, w, h, COLORS.green)?;
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
    let gpu = &sysmon.gpu;
    let x = OUTER_MARGIN;
    let mut y = OUTER_MARGIN;
    if let Some(cr) = cr {
        paint_header(cr, text, gpu, (x, y, w))?;
    }
    y += HEADER_H;
    if !gpu.available {
        if let Some(cr) = cr {
            let line = "NVML unavailable -- no NVIDIA driver?";
            text.draw_left(cr, (x, y, ROW_H), line, FONT.sm, false, COLORS.subtext0)?;
        }
        return Ok(y + ROW_H + OUTER_MARGIN);
    }
    y += SECTION_GAP;
    y += paint_graph_card(cr, text, sysmon, (x, y, w))?;
    if let Some(cr) = cr {
        stat_card::draw_axis(cr, text, (x, y, w), stat_card::graph_cells(w, HISTORY_LEN))?;
    }
    y += stat_card::axis_height() + SECTION_GAP;
    y += paint_rows(cr, text, gpu, (x, y))?;
    y += SECTION_GAP;
    y += paint_apps(cr, text, gpu, (x, y, w))?;
    Ok(y + OUTER_MARGIN)
}

/// The P-state pill's word and colour: P0-2 busy, P3-7 active, P8+ idle.
const fn pstate_pill(pstate: u8) -> (&'static str, Color) {
    if pstate <= 2 {
        ("busy", COLORS.peach)
    } else if pstate <= 7 {
        ("active", COLORS.yellow)
    } else {
        ("idle", COLORS.green)
    }
}

/// The header row (or the "no GPU" one).
fn paint_header(
    cr: &cairo::Context,
    text: &mut TextCache,
    gpu: &Gpu,
    origin: (f64, f64, f64),
) -> Result<(), cairo::Error> {
    if !gpu.available {
        let header = Header {
            icon: Some(ICON_GPU),
            icon_color: COLORS.green,
            title: NO_GPU,
            summary: "",
            pill: None,
            pill_fill: COLORS.green,
        };
        return popup_chrome::draw_header(cr, text, origin, &header);
    }
    let driver = if gpu.driver.is_empty() {
        String::new()
    } else {
        format!("  \u{b7}  driver {}", gpu.driver)
    };
    let pill = gpu
        .pstate
        .map(|p| (format!("P{p} {}", pstate_pill(p).0), pstate_pill(p).1));
    let header = Header {
        icon: Some(ICON_GPU),
        icon_color: COLORS.green,
        title: short_name(&gpu.name),
        summary: &driver,
        pill: pill.as_ref().map(|(word, _)| word.as_str()),
        pill_fill: pill.as_ref().map_or(COLORS.green, |(_, fill)| *fill),
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

    let gpu = &sysmon.gpu;
    let history = &sysmon.gpu_history;
    let (first, count) = stat_card::history_window(history.len(), cells);
    let usage: Vec<f64> = (first..first + count).map(|i| history.a(i)).collect();
    let temps: Vec<f64> = (first..first + count).map(|i| history.b(i)).collect();
    let temp_dots: Vec<f64> = temps
        .iter()
        .map(|&t| (t - TEMP_GRAPH_MIN_C).max(0.0))
        .collect();
    let usage_peak = usage.iter().copied().fold(0.0, f64::max);
    let temp_peak = temps.iter().copied().fold(0.0, f64::max);

    let (inner_x, inner_w) = (x + CARD_PAD, w - CARD_PAD * 2.0);
    let graph_x = x + (w - braille_graph::width(cells)) / 2.0;
    let mut cy = y + CARD_PAD;
    let usage_label = format!("usage {}%", gpu.percent);
    let usage_peak_label = format!("peak {usage_peak:.0}%");
    stat_card::graph_label_row(
        cr,
        text,
        (inner_x, cy, inner_w),
        &usage_label,
        COLORS.green,
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

    let temp_label = format!("temp {}\u{b0}C", gpu.temp_c);
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

/// The VRAM, POWER and FAN values.
fn meter_texts(gpu: &Gpu) -> [String; 3] {
    let to_gb = |mb: u64| f64::from(u32::try_from(mb).unwrap_or(u32::MAX)) / 1024.0;
    let vram = format!(
        "{:.1} / {:.1} GB",
        to_gb(gpu.mem_used_mb),
        to_gb(gpu.mem_total_mb)
    );
    let power = if gpu.power_limit_w > 0.0 {
        format!("{:.0} / {:.0} W", gpu.power_w, gpu.power_limit_w)
    } else {
        format!("{:.0} W", gpu.power_w)
    };
    let fan = if gpu.fan_percent == 0 {
        "0% \u{b7} stopped".to_owned()
    } else {
        format!("{}%", gpu.fan_percent)
    };
    [vram, power, fan]
}

/// The CLOCK and VIDEO lines.
fn line_texts(gpu: &Gpu) -> [String; 2] {
    let clock = if gpu.clock_max_mhz > 0 {
        format!(
            "{} / {} MHz \u{b7} mem {} MHz",
            gpu.clock_mhz, gpu.clock_max_mhz, gpu.mem_clock_mhz
        )
    } else {
        format!("{} MHz \u{b7} mem {} MHz", gpu.clock_mhz, gpu.mem_clock_mhz)
    };
    let video = format!(
        "enc {}% \u{b7} dec {}%",
        gpu.encoder_percent, gpu.decoder_percent
    );
    [clock, video]
}

/// VRAM, POWER, FAN meters, then CLOCK and VIDEO. Returns the height.
fn paint_rows(
    cr: Option<&cairo::Context>,
    text: &mut TextCache,
    gpu: &Gpu,
    (x, y): (f64, f64),
) -> Result<f64, cairo::Error> {
    let h = ROW_H * 5.0;
    let Some(cr) = cr else {
        return Ok(h);
    };
    let value_x = x + stat_card::label_col_w(text, &ROW_LABELS) + COL_GAP;
    let ratio = |value: f64, whole: f64| if whole > 0.0 { value / whole } else { 0.0 };
    let as_f64 = |mb: u64| f64::from(u32::try_from(mb).unwrap_or(u32::MAX));
    let fractions = [
        ratio(as_f64(gpu.mem_used_mb), as_f64(gpu.mem_total_mb)),
        ratio(gpu.power_w, gpu.power_limit_w),
        f64::from(gpu.fan_percent) / 100.0,
    ];
    let mut ry = y;
    for ((label, fraction), value) in ["VRAM", "POWER", "FAN"]
        .into_iter()
        .zip(fractions)
        .zip(meter_texts(gpu))
    {
        text.draw_left(cr, (x, ry, ROW_H), label, FONT.xs, true, COLORS.overlay0)?;
        let meter = BoxRect {
            x: value_x,
            y: ry + (ROW_H - METER_H) / 2.0,
            w: METER_W,
            h: METER_H,
        };
        meter_bar::draw(cr, meter, fraction, METER_SEGMENTS, &HEAT, COLORS.surface0)?;
        text.draw_left(
            cr,
            (value_x + METER_W + COL_GAP, ry, ROW_H),
            &value,
            FONT.label,
            false,
            COLORS.text,
        )?;
        ry += ROW_H;
    }
    for (label, value) in ["CLOCK", "VIDEO"].into_iter().zip(line_texts(gpu)) {
        stat_card::draw_kv_row(cr, text, (x, value_x, ry), label, &value)?;
        ry += ROW_H;
    }
    Ok(h)
}

/// TOP VRAM APPS: always `MAX_PROCS` rows tall. Returns the height.
fn paint_apps(
    cr: Option<&cairo::Context>,
    text: &mut TextCache,
    gpu: &Gpu,
    (x, y, w): (f64, f64, f64),
) -> Result<f64, cairo::Error> {
    let h = ROW_H * (1.0 + f64::from(u32::try_from(MAX_PROCS).unwrap_or(0)));
    let Some(cr) = cr else {
        return Ok(h);
    };
    text.draw_left(
        cr,
        (x, y, ROW_H),
        "TOP VRAM APPS",
        FONT.xs,
        true,
        COLORS.overlay0,
    )?;
    if gpu.procs.is_empty() {
        let note = "no apps using VRAM";
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
    let as_f64 = |mib: u64| f64::from(u32::try_from(mib).unwrap_or(u32::MAX));
    let biggest = gpu.procs.first().map_or(0.0, |(_, mib)| as_f64(*mib));
    let mut ry = y;
    for (name, mib) in &gpu.procs {
        ry += ROW_H;
        let fraction = if biggest > 0.0 {
            as_f64(*mib) / biggest
        } else {
            0.0
        };
        stat_card::draw_app_row(
            cr,
            text,
            (x, ry, w),
            name,
            fraction,
            COLORS.green,
            &format!("{mib} MiB"),
        )?;
    }
    Ok(h)
}

/// The content column's width: the graph's minimum, the header, and the
/// widest row (with worst-case sample strings), as in C.
fn content_width(text: &mut TextCache, gpu: &Gpu) -> f64 {
    let graph_w = braille_graph::width(GRAPH_MIN_CELLS_W) + CARD_PAD * 2.0;
    if !gpu.available {
        let header = Header {
            icon: Some(ICON_GPU),
            icon_color: COLORS.green,
            title: NO_GPU,
            summary: "",
            pill: None,
            pill_fill: COLORS.green,
        };
        return graph_w.max(popup_chrome::header_width(text, &header));
    }
    let driver = format!("  \u{b7}  driver {}", gpu.driver);
    let header = Header {
        icon: Some(ICON_GPU),
        icon_color: COLORS.green,
        title: short_name(&gpu.name),
        summary: &driver,
        pill: Some("P12 active"),
        pill_fill: COLORS.yellow,
    };
    let header_w = popup_chrome::header_width(text, &header);
    let value_x = stat_card::label_col_w(text, &ROW_LABELS) + COL_GAP;
    let [vram, power, fan] = meter_texts(gpu);
    let meters_w = [vram.as_str(), &power, &fan, "000 / 000 W", "100%"]
        .iter()
        .map(|value| value_x + METER_W + COL_GAP + text.width(value, FONT.label, false))
        .fold(0.0, f64::max);
    let [clock, video] = line_texts(gpu);
    let lines_w = [
        clock.as_str(),
        &video,
        "0000 / 0000 MHz \u{b7} mem 00000 MHz",
    ]
    .iter()
    .map(|value| value_x + text.width(value, FONT.label, false))
    .fold(0.0, f64::max);
    let apps_w = stat_card::app_name_col_w(text)
        + stat_card::APP_BAR_W
        + COL_GAP
        + text.width("00000 MiB", FONT.label, false);
    [graph_w, header_w, meters_w, lines_w, apps_w]
        .into_iter()
        .fold(0.0, f64::max)
}
