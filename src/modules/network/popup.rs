//! The network dropdown (the C bar's `modules/network/network_info_popup.c`;
//! `docs/modules/network.md` describes each part). The sysmon dropdowns'
//! twin, anchored to the right since the box is in the bar's right region.
//!
//! Top to bottom:
//! - header: status icon, interface name, `· Ethernet`/`· Wi-Fi`, and a
//!   `connected` (teal) / `down` (red) pill; just "No active connection"
//!   without a default route (and nothing below it);
//! - a graph card: the download rate rising to a centre line, the upload rate
//!   hanging below it, each half scaled to its own peak with a 10 KB/s floor
//!   (so an idle trickle stays a row of single dots), then the time axis;
//! - a details grid: IP, GATEWAY, MAC, MTU, LINK (Ethernet only), SINCE BOOT;
//! - DNS servers as small tags that wrap.
//!
//! Sections take `Option<&cairo::Context>`: `None` only measures (the
//! open-time height pass), as in the sysmon dropdowns.

use std::fs::File;

use smithay_client_toolkit::reexports::client::protocol::wl_output::WlOutput;

use super::status_glyph;
use crate::{
    bar::{
        popup_host::PopupHost,
        popup_surface::{PopupEnv, PopupSurface, render_image},
    },
    config::{
        COLORS, Color, FONT,
        glyphs::{ARROW_DOWN, ARROW_UP},
    },
    network::{Network, format_bytes, format_rate},
    render::{
        BoxRect, braille_graph,
        module_box::draw_rect,
        popup_chrome::{self, Header},
        popup_style::{HEADER_H, OUTER_MARGIN, SECTION_GAP},
        stat_card::{self, CARD_PAD, GRAPH_LABEL_GAP, GRAPH_LABEL_H, GRAPH_MIN_CELLS_W, ROW_H},
        text::TextCache,
        whole_px,
    },
    util::history::HISTORY_LEN,
};

/// Between the download and upload halves (the centre line sits in it).
const GRAPH_CENTER_GAP: f64 = 5.0;
/// Each half's height in cells.
const GRAPH_CELLS_H: u32 = 4;
/// Grid label column to values.
const GRID_COL_GAP: f64 = 12.0;
/// A DNS tag's height.
const TAG_H: f64 = 16.0;
/// A DNS tag's horizontal padding, each side.
const TAG_PAD_X: f64 = 6.0;
/// Between DNS tags, both directions.
const TAG_GAP: f64 = 5.0;
/// The graphs never scale below 10 KB/s.
const GRAPH_MIN_SCALE: f64 = 10.0 * 1024.0;
/// Every grid label (for the label column's width).
const GRID_LABELS: [&str; 7] = ["IP", "GATEWAY", "MAC", "MTU", "LINK", "SINCE BOOT", "DNS"];
/// Download colouring: teal → green → yellow.
const RX_FILL: [Color; 3] = [COLORS.teal, COLORS.green, COLORS.yellow];
/// Upload colouring: mauve → pink → red.
const TX_FILL: [Color; 3] = [COLORS.mauve, COLORS.pink, COLORS.red];
/// The header's title without a default route.
const NO_CONNECTION: &str = "No active connection";

/// The network dropdown: closed (`host` is `None`) or open.
#[derive(Default)]
pub(crate) struct NetworkPopup {
    /// The open popup's surfaces.
    host: Option<PopupHost>,
    /// The popup's size, fixed while open (only an interface or DNS-count
    /// change while open could change the layout; it then clips until reopened).
    size: (f64, f64),
}

impl NetworkPopup {
    /// Opens the dropdown under the box `trigger` on `output` (`output_width`
    /// px wide), re-opening it if it's open: fresh details, size, paint.
    pub(crate) fn open<'o>(
        &mut self,
        env: &PopupEnv<'_>,
        (output, output_width): (&WlOutput, i32),
        trigger: BoxRect,
        all_outputs: impl Iterator<Item = &'o WlOutput>,
        network: &mut Network,
        text: &mut TextCache,
    ) {
        network.refresh_details();
        self.host = None;
        let content_w = content_width(text, network);
        let height = paint_content(None, text, network, content_w).unwrap_or(0.0);
        self.size = (content_w + OUTER_MARGIN * 2.0, height);
        let size = (whole_px(self.size.0), whole_px(self.size.1));
        self.host = Some(PopupHost::open_right(
            env,
            (output, output_width),
            trigger,
            size,
            all_outputs,
        ));
        self.paint(network, text);
    }

    /// The 5 s poll ran: refresh the details and repaint, if open.
    pub(crate) fn refresh_if_open(&mut self, network: &mut Network, text: &mut TextCache) {
        if self.host.is_some() {
            network.refresh_details();
            self.paint(network, text);
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
    pub(crate) fn paint(&mut self, network: &Network, text: &mut TextCache) {
        let size = self.size;
        if let Some(host) = self.host.as_mut() {
            host.menu_mut().paint(|cr| draw(cr, text, network, size));
        }
    }

    /// Writes what the open popup shows, drawn again, to a PNG (the tests' `snapshot`).
    pub(crate) fn snapshot(
        &self,
        path: &str,
        network: &Network,
        text: &mut TextCache,
    ) -> Result<(), String> {
        let host = self.host.as_ref().ok_or("no open menu to snapshot")?;
        let (width, height) = host.menu().menu_size().ok_or("not a menu")?;
        let size = self.size;
        let image = render_image(width, height, |cr| draw(cr, text, network, size))
            .map_err(|e| e.to_string())?;
        let mut file = File::create(path).map_err(|e| e.to_string())?;
        image.write_to_png(&mut file).map_err(|e| e.to_string())
    }
}

/// The whole popup: frame, then the content.
fn draw(
    cr: &cairo::Context,
    text: &mut TextCache,
    network: &Network,
    (w, h): (f64, f64),
) -> Result<(), cairo::Error> {
    popup_chrome::draw_frame(cr, w, h, COLORS.teal)?;
    paint_content(Some(cr), text, network, w - OUTER_MARGIN * 2.0).map(|_height| ())
}

/// Every section, top to bottom, in a content column `w` wide. Returns the
/// popup's total height (also when only measuring).
fn paint_content(
    cr: Option<&cairo::Context>,
    text: &mut TextCache,
    network: &Network,
    w: f64,
) -> Result<f64, cairo::Error> {
    let x = OUTER_MARGIN;
    let mut y = OUTER_MARGIN;
    if let Some(cr) = cr {
        let header_text = HeaderText::for_network(network);
        popup_chrome::draw_header(cr, text, (x, y, w), &header_text.header(network))?;
    }
    y += HEADER_H;
    if !network.iface.is_empty() {
        y += SECTION_GAP;
        y += paint_graph_card(cr, text, network, (x, y, w))?;
        if let Some(cr) = cr {
            stat_card::draw_axis(cr, text, (x, y, w), stat_card::graph_cells(w, HISTORY_LEN))?;
        }
        y += stat_card::axis_height() + SECTION_GAP;
        y += paint_details(cr, text, network, (x, y, w))?;
    }
    Ok(y + OUTER_MARGIN)
}

/// The header's text, owned so the `Header` can borrow it.
struct HeaderText {
    /// `  · Ethernet` / `  · Wi-Fi` ("" without a connection).
    summary: &'static str,
}

impl HeaderText {
    /// The header text for `network`.
    const fn for_network(network: &Network) -> Self {
        Self {
            summary: if network.wifi {
                "  \u{b7}  Wi-Fi"
            } else {
                "  \u{b7}  Ethernet"
            },
        }
    }

    /// The header row's content.
    fn header<'a>(&'a self, network: &'a Network) -> Header<'a> {
        let icon = Some(status_glyph(network));
        if network.iface.is_empty() {
            return Header {
                icon,
                icon_color: COLORS.teal,
                title: NO_CONNECTION,
                summary: "",
                pill: None,
                pill_fill: COLORS.teal,
            };
        }
        let (pill, fill) = if network.connected {
            ("connected", COLORS.teal)
        } else {
            ("down", COLORS.red)
        };
        Header {
            icon,
            icon_color: COLORS.teal,
            title: &network.iface,
            summary: self.summary,
            pill: Some(pill),
            pill_fill: fill,
        }
    }
}

/// One rate label row above or below a graph half: `↓ 12.3 KB/s ... peak 40 KB/s`.
fn rate_label_row(
    cr: &cairo::Context,
    text: &mut TextCache,
    origin: (f64, f64, f64),
    (download, rate, peak): (bool, f64, f64),
) -> Result<(), cairo::Error> {
    let arrow = if download { ARROW_DOWN } else { ARROW_UP };
    let label = format!("{arrow} {}", format_rate(rate));
    let peak = format!("peak {}", format_rate(peak));
    let color = if download { COLORS.teal } else { COLORS.mauve };
    stat_card::graph_label_row(cr, text, origin, &label, color, &peak)
}

/// The two-half rate graph card. Returns its height.
fn paint_graph_card(
    cr: Option<&cairo::Context>,
    text: &mut TextCache,
    network: &Network,
    (x, y, w): (f64, f64, f64),
) -> Result<f64, cairo::Error> {
    let cells = stat_card::graph_cells(w, HISTORY_LEN);
    let half_h = braille_graph::height(GRAPH_CELLS_H);
    let h = CARD_PAD * 2.0
        + GRAPH_LABEL_H * 2.0
        + GRAPH_LABEL_GAP * 2.0
        + half_h * 2.0
        + GRAPH_CENTER_GAP;
    let Some(cr) = cr else {
        return Ok(h);
    };
    popup_chrome::draw_card(cr, BoxRect { x, y, w, h }, false, false, COLORS.surface1)?;

    let history = &network.history;
    let (first, count) = stat_card::history_window(history.len(), cells);
    let rx: Vec<f64> = (first..first + count).map(|i| history.a(i)).collect();
    let tx: Vec<f64> = (first..first + count).map(|i| history.b(i)).collect();
    let rx_peak = rx.iter().copied().fold(0.0, f64::max);
    let tx_peak = tx.iter().copied().fold(0.0, f64::max);

    let (inner_x, inner_w) = (x + CARD_PAD, w - CARD_PAD * 2.0);
    let graph_w = braille_graph::width(cells);
    let graph_x = x + (w - graph_w) / 2.0;
    let mut cy = y + CARD_PAD;
    rate_label_row(
        cr,
        text,
        (inner_x, cy, inner_w),
        (true, network.rx_rate, rx_peak),
    )?;
    cy += GRAPH_LABEL_H + GRAPH_LABEL_GAP;
    // Download rises to the centre line...
    braille_graph::draw(
        cr,
        (graph_x, cy),
        (cells, GRAPH_CELLS_H),
        &rx,
        rx_peak.max(GRAPH_MIN_SCALE),
        true,
        &RX_FILL,
    )?;
    cy += half_h;
    cr.save()?;
    COLORS.surface1.set_source(cr);
    cr.rectangle(graph_x, cy + GRAPH_CENTER_GAP / 2.0 - 0.5, graph_w, 1.0);
    cr.fill()?;
    cr.restore()?;
    cy += GRAPH_CENTER_GAP;
    // ...and upload hangs below it.
    braille_graph::draw(
        cr,
        (graph_x, cy),
        (cells, GRAPH_CELLS_H),
        &tx,
        tx_peak.max(GRAPH_MIN_SCALE),
        false,
        &TX_FILL,
    )?;
    cy += half_h + GRAPH_LABEL_GAP;
    rate_label_row(
        cr,
        text,
        (inner_x, cy, inner_w),
        (false, network.tx_rate, tx_peak),
    )?;
    Ok(h)
}

/// The grid's (label, value) rows; LINK only on Ethernet.
fn grid_rows(network: &Network) -> Vec<(&'static str, String)> {
    let or_dash = |s: &str| {
        if s.is_empty() {
            "-".to_owned()
        } else {
            s.to_owned()
        }
    };
    let mut rows = vec![
        ("IP", or_dash(&network.ip)),
        ("GATEWAY", or_dash(&network.gateway)),
        ("MAC", or_dash(&network.mac)),
        ("MTU", or_dash(&network.mtu)),
    ];
    if !network.wifi {
        let link = if network.speed.is_empty() {
            "-".to_owned()
        } else {
            format!(
                "{} Mbps, {} duplex",
                network.speed,
                or_dash(&network.duplex)
            )
        };
        rows.push(("LINK", link));
    }
    let totals = format!(
        "{ARROW_DOWN} {}   {ARROW_UP} {}",
        format_bytes(network.rx_total),
        format_bytes(network.tx_total)
    );
    rows.push(("SINCE BOOT", totals));
    rows
}

/// The details grid, then the DNS tags. Returns the height.
fn paint_details(
    cr: Option<&cairo::Context>,
    text: &mut TextCache,
    network: &Network,
    (x, y, w): (f64, f64, f64),
) -> Result<f64, cairo::Error> {
    let value_x = x + stat_card::label_col_w(text, &GRID_LABELS) + GRID_COL_GAP;
    let mut cy = y;
    for (label, value) in grid_rows(network) {
        if let Some(cr) = cr {
            text.draw_left(cr, (x, cy, ROW_H), label, FONT.xs, true, COLORS.overlay0)?;
            text.draw_left(
                cr,
                (value_x, cy, ROW_H),
                &value,
                FONT.label,
                false,
                COLORS.text,
            )?;
        }
        cy += ROW_H;
    }
    if let Some(cr) = cr {
        text.draw_left(cr, (x, cy, ROW_H), "DNS", FONT.xs, true, COLORS.overlay0)?;
    }
    cy += paint_dns_tags(cr, text, &network.dns, (value_x, cy, x + w - value_x))?;
    Ok(cy - y)
}

/// The DNS servers as tags, wrapping onto new rows within `w`. Returns the height.
fn paint_dns_tags(
    cr: Option<&cairo::Context>,
    text: &mut TextCache,
    dns: &[String],
    (x, y, w): (f64, f64, f64),
) -> Result<f64, cairo::Error> {
    let (mut row_y, mut tag_x) = (0.0, 0.0);
    for server in dns {
        let tag_w = text.width(server, FONT.sm, false) + TAG_PAD_X * 2.0;
        if tag_x > 0.0 && tag_x + tag_w > w {
            tag_x = 0.0;
            row_y += TAG_H + TAG_GAP;
        }
        if let Some(cr) = cr {
            let tag = BoxRect {
                x: x + tag_x,
                y: y + row_y + (ROW_H - TAG_H) / 2.0,
                w: tag_w,
                h: TAG_H,
            };
            draw_rect(cr, tag, COLORS.surface0, COLORS.surface1, 1.0)?;
            text.draw_centered(cr, tag, server, FONT.sm, false, COLORS.text)?;
        }
        tag_x += tag_w + TAG_GAP;
    }
    if dns.is_empty()
        && let Some(cr) = cr
    {
        text.draw_left(cr, (x, y, ROW_H), "-", FONT.label, false, COLORS.text)?;
    }
    Ok(row_y + ROW_H)
}

/// The content column's width: the graph's minimum, the header and the
/// widest grid row.
fn content_width(text: &mut TextCache, network: &Network) -> f64 {
    let graph_w = braille_graph::width(GRAPH_MIN_CELLS_W) + CARD_PAD * 2.0;
    let header_text = HeaderText {
        summary: "  \u{b7}  Ethernet",
    };
    let mut header = header_text.header(network);
    if !network.iface.is_empty() {
        header.pill = Some("connected");
    }
    let header_w = popup_chrome::header_width(text, &header);
    let value_x = stat_card::label_col_w(text, &GRID_LABELS) + GRID_COL_GAP;
    let rows_w = grid_rows(network)
        .iter()
        .map(|(_, value)| value_x + text.width(value, FONT.label, false))
        .fold(0.0, f64::max);
    [graph_w, header_w, rows_w].into_iter().fold(0.0, f64::max)
}
