//! The Proxmox dropdown (`docs/modules/proxmox.md`). Its accent follows the
//! box's colour (green up, grey off, yellow waking, peach shutting down, red
//! error).
//!
//! - **Header:** the server icon, `Proxmox · pve` and a pill: `up 3d 4h`,
//!   `off`, `waking`, `shutting down`, `error` or `...` (no answer yet).
//! - **A red line** when the last poll failed, or the last Wake/Shut down did.
//! - **While off or waking:** a note line (`no answer from 192.168.0.50`, or
//!   how long ago the packet went) and the **Wake** chip. It sends the magic
//!   packet; while waking it reads `Send again`.
//! - **While shutting down:** a note (how long ago it was sent), no chip.
//! - **While up** (and through an error, with the last figures):
//!   - one card per node (CPU, RAM and root-disk meters, its uptime);
//!   - a `GUESTS` card with one row per VM/CT (state dot, `VM`/`CT`, id,
//!     name; CPU % and RAM while running), at most [`MAX_GUEST_ROWS`], then
//!     `+N more`;
//!   - a `STORAGE` card, one row per storage with used/total;
//!   - (only while up) the **Shut down** chip at the very bottom, away from
//!     everything else. It takes two clicks: the first **arms** it (filled
//!     red, `Click again to shut down`), and a second within [`ARM_WINDOW`]
//!     sends it. Otherwise it disarms (on the 3 s tick), and closing the
//!     dropdown disarms it too. Shutting down stops every guest, so one stray
//!     click must never do it.
//!
//! The chip is the only clickable thing (hand cursor). Opened on right-click,
//! anchored under the box's left edge and growing rightwards, like the other
//! left-region dropdowns (CPU, GPU, memory). Every finished poll re-lays it
//! out, recreating the surface on a height change (the updates dropdown's
//! rule). While it's open, polls run every 3 s instead of 30 s.

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
    config::{
        COLORS, Color, FONT,
        glyphs::{ICON_POWER, ICON_SERVER},
    },
    proxmox::{
        Phase, Proxmox,
        resources::{Guest, GuestKind, GuestState, Node, Storage},
    },
    render::{
        BoxRect, meter_bar,
        module_box::draw_rect,
        popup_chrome::{self, Header},
        popup_style::{CARD_GAP, COL_GAP, HEADER_H, OUTER_MARGIN, SECTION_GAP},
        text::TextCache,
        whole_px,
    },
};

// Layout, local to this popup (CLAUDE.md "Conventions").
/// The content column's width (fixed: long names are shortened instead).
const CONTENT_W: f64 = 400.0;
/// Card edge to its content.
const CARD_PAD: f64 = 8.0;
/// A card's title row.
const CARD_TITLE_H: f64 = 18.0;
/// One row inside a card.
const ROW_H: f64 = 18.0;
/// A node card's meter rows (CPU, RAM, disk).
const NODE_ROWS: usize = 3;
/// The error line or the off/waking note under the header.
const NOTE_ROW_H: f64 = 16.0;
/// The Wake chip's height.
const CHIP_H: f64 = 28.0;
/// The most node cards (a homelab has one; a big cluster isn't this popup's job).
const MAX_NODES: usize = 4;
/// The most guest rows; the rest are `+N more`.
pub(crate) const MAX_GUEST_ROWS: usize = 20;
/// The most storage rows; the rest are `+N more`.
const MAX_STORAGE_ROWS: usize = 8;
/// A node card's row label column (`CPU`, `RAM`, `Disk`).
const LABEL_W: f64 = 40.0;
/// A node card's meter width.
const METER_W: f64 = 150.0;
/// A meter's height (centered in its row).
const METER_H: f64 = 8.0;
/// Segments per meter.
const METER_SEGMENTS: u32 = 20;
/// Meter colouring, low to high (the CPU dropdown's).
const HEAT: [Color; 3] = [COLORS.green, COLORS.yellow, COLORS.red];
/// A guest row's columns, from the card's inner left: the state dot, the
/// `VM`/`CT` tag, the id, the name.
const TAG_X: f64 = 14.0;
/// See [`TAG_X`].
const ID_X: f64 = 36.0;
/// See [`TAG_X`].
const NAME_X: f64 = 76.0;
/// How long an armed Shut down chip waits for its second click.
const ARM_WINDOW: Duration = Duration::from_secs(5);
/// One gibibyte / tebibyte.
const GIB: f64 = 1024.0 * 1024.0 * 1024.0;
/// See [`GIB`].
const TIB: f64 = GIB * 1024.0;

/// A list card's place and how many rows it shows.
#[derive(Clone, Copy, Debug)]
struct Section {
    /// Its top.
    y: f64,
    /// Rows shown.
    rows: usize,
    /// Items not shown (the `+N more` row when > 0).
    more: usize,
}

impl Section {
    /// Its height: a title, the rows, `+N more`, or one row saying "none".
    fn height(self) -> f64 {
        let rows = (self.rows + usize::from(self.more > 0)).max(1);
        CARD_PAD * 2.0 + CARD_TITLE_H + rows_h(rows)
    }
}

/// Which action the dropdown's chip is.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ChipKind {
    /// Wake-on-LAN (while off or waking).
    Wake,
    /// Shut the node down (while up; two clicks).
    Shutdown,
}

/// What a press on the menu did.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ChipPress {
    /// The Wake chip: send the magic packet.
    Wake,
    /// The Shut down chip's first click: it's armed now (repaint).
    Armed,
    /// The Shut down chip's second click: shut down.
    Shutdown,
}

/// Where everything goes, for one state of the data.
#[derive(Clone, Debug)]
struct Layout {
    /// The surface's size.
    size: (f64, f64),
    /// The red error line's top, if one is shown.
    error_y: Option<f64>,
    /// The off/waking note's top, if shown.
    note_y: Option<f64>,
    /// The chip and which one it is, if shown.
    chip: Option<(BoxRect, ChipKind)>,
    /// Each node card's top (the first [`MAX_NODES`] nodes).
    nodes: Vec<f64>,
    /// The guests card.
    guests: Option<Section>,
    /// The storage card.
    storage: Option<Section>,
}

/// `n` rows' height.
fn rows_h(n: usize) -> f64 {
    // A handful of rows: the conversion can't fail.
    f64::from(u32::try_from(n).unwrap_or(0)) * ROW_H
}

/// Lays the dropdown out for `proxmox` as it is now.
fn layout(proxmox: &Proxmox) -> Layout {
    let mut y = OUTER_MARGIN + HEADER_H + SECTION_GAP;
    let mut out = Layout {
        size: (0.0, 0.0),
        error_y: None,
        note_y: None,
        chip: None,
        nodes: Vec::new(),
        guests: None,
        storage: None,
    };
    let phase = proxmox.phase();
    if error_line(proxmox).is_some() {
        out.error_y = Some(y);
        y += NOTE_ROW_H + CARD_GAP;
    }
    if matches!(
        phase,
        Phase::Off | Phase::Waking | Phase::Unknown | Phase::ShuttingDown
    ) {
        out.note_y = Some(y);
        y += NOTE_ROW_H + CARD_GAP;
    }
    if matches!(phase, Phase::Off | Phase::Waking | Phase::Unknown) {
        out.chip = Some((chip_rect(y), ChipKind::Wake));
        y += CHIP_H + CARD_GAP;
    }
    if let Some(cluster) = &proxmox.cluster {
        for _node in cluster.nodes.iter().take(MAX_NODES) {
            out.nodes.push(y);
            y += node_card_h() + CARD_GAP;
        }
        for (section, total, cap) in [
            (&mut out.guests, cluster.guests.len(), MAX_GUEST_ROWS),
            (&mut out.storage, cluster.storages.len(), MAX_STORAGE_ROWS),
        ] {
            let rows = total.min(cap);
            let card = Section {
                y,
                rows,
                more: total - rows,
            };
            y += card.height() + CARD_GAP;
            *section = Some(card);
        }
        // At the very bottom, away from everything else (it stops every guest).
        if phase == Phase::Up && !cluster.nodes.is_empty() {
            out.chip = Some((chip_rect(y), ChipKind::Shutdown));
            y += CHIP_H + CARD_GAP;
        }
    }
    // The last block's gap isn't needed.
    y -= CARD_GAP;
    out.size = (CONTENT_W + OUTER_MARGIN * 2.0, y + OUTER_MARGIN);
    out
}

/// A chip, full width, at `y`.
const fn chip_rect(y: f64) -> BoxRect {
    BoxRect {
        x: OUTER_MARGIN,
        y,
        w: CONTENT_W,
        h: CHIP_H,
    }
}

/// The red line under the header: why the last Wake/Shut down failed, or
/// else why the last poll did (while that's the state).
fn error_line(proxmox: &Proxmox) -> Option<&str> {
    proxmox.action_error.as_deref().or_else(|| {
        if proxmox.phase() == Phase::Error {
            proxmox.last_error.as_deref()
        } else {
            None
        }
    })
}

/// A node card's height.
fn node_card_h() -> f64 {
    CARD_PAD * 2.0 + CARD_TITLE_H + rows_h(NODE_ROWS)
}

/// An open dropdown.
struct Open {
    /// Its surfaces.
    host: PopupHost,
    /// Where everything is.
    layout: Layout,
    /// When the Shut down chip was armed (its first click), while armed.
    armed: Option<Instant>,
}

/// The Proxmox dropdown: closed or open.
#[derive(Default)]
pub(crate) struct ProxmoxPopup {
    /// The open dropdown, if any.
    open: Option<Open>,
}

impl ProxmoxPopup {
    /// Opens the dropdown under the box `trigger` on `output`, replacing an
    /// open one.
    pub(crate) fn open<'o>(
        &mut self,
        env: &PopupEnv<'_>,
        output: &WlOutput,
        trigger: BoxRect,
        all_outputs: impl Iterator<Item = &'o WlOutput>,
        proxmox: &Proxmox,
        text: &mut TextCache,
    ) {
        self.open = None;
        let layout = layout(proxmox);
        let host = PopupHost::open_left(
            env,
            output,
            trigger,
            (whole_px(layout.size.0), whole_px(layout.size.1)),
            all_outputs,
        );
        self.open = Some(Open {
            host,
            layout,
            armed: None,
        });
        self.paint(proxmox, text);
    }

    /// A poll finished (or waking began): re-lays out, recreating the
    /// surface if the height changed.
    pub(crate) fn refresh(&mut self, env: &PopupEnv<'_>, proxmox: &Proxmox, text: &mut TextCache) {
        let Some(open) = self.open.as_mut() else {
            return;
        };
        let layout = layout(proxmox);
        if whole_px(layout.size.1) != whole_px(open.layout.size.1) {
            open.host
                .resize(env, (whole_px(layout.size.0), whole_px(layout.size.1)));
        }
        // The Shut down chip went away (it's no longer up): nothing to arm.
        if layout.chip.map(|(_, kind)| kind) != Some(ChipKind::Shutdown) {
            open.armed = None;
        }
        open.layout = layout;
        self.paint(proxmox, text);
    }

    /// A press at (x, y) on the menu: what the chip under it does, if any.
    /// The Shut down chip's first click (or one after [`ARM_WINDOW`]) only
    /// arms it; the caller repaints.
    pub(crate) fn press(&mut self, x: f64, y: f64) -> Option<ChipPress> {
        let open = self.open.as_mut()?;
        let (rect, kind) = open.layout.chip?;
        if !rect.contains(x, y) {
            return None;
        }
        Some(match kind {
            ChipKind::Wake => ChipPress::Wake,
            ChipKind::Shutdown => {
                if open
                    .armed
                    .take()
                    .is_some_and(|since| since.elapsed() < ARM_WINDOW)
                {
                    ChipPress::Shutdown
                } else {
                    open.armed = Some(Instant::now());
                    ChipPress::Armed
                }
            }
        })
    }

    /// Disarms a Shut down chip armed longer than [`ARM_WINDOW`] ago (the 3 s
    /// tick). Returns whether it did, so the caller repaints.
    pub(crate) fn expire_arm(&mut self) -> bool {
        let Some(open) = self.open.as_mut() else {
            return false;
        };
        let expired = open
            .armed
            .is_some_and(|since| since.elapsed() >= ARM_WINDOW);
        if expired {
            open.armed = None;
        }
        expired
    }

    /// Whether the Shut down chip is armed (the tests' `menu`).
    pub(crate) fn armed(&self) -> bool {
        self.open.as_ref().is_some_and(|o| o.armed.is_some())
    }

    /// Closes the dropdown (no-op if closed).
    pub(crate) fn close(&mut self) {
        self.open = None;
    }

    /// The open dropdown's surfaces.
    pub(crate) fn host(&self) -> Option<&PopupHost> {
        self.open.as_ref().map(|o| &o.host)
    }

    /// The open dropdown's surfaces, for configure routing.
    pub(crate) fn host_mut(&mut self) -> Option<&mut PopupHost> {
        self.open.as_mut().map(|o| &mut o.host)
    }

    /// The menu surface, if open.
    pub(crate) fn menu(&self) -> Option<&PopupSurface> {
        self.host().map(PopupHost::menu)
    }

    /// Whether (x, y) on the menu is over the chip (the hand cursor).
    pub(crate) fn over_chip(&self, x: f64, y: f64) -> bool {
        self.chip().is_some_and(|(rect, _)| rect.contains(x, y))
    }

    /// The chip and which one it is (the tests click its middle), `None`
    /// when not shown.
    pub(crate) fn chip(&self) -> Option<(BoxRect, ChipKind)> {
        self.open.as_ref().and_then(|o| o.layout.chip)
    }

    /// How many guest rows are shown (the tests' `menu`), `None` when closed.
    pub(crate) fn guest_rows(&self) -> Option<usize> {
        self.open
            .as_ref()
            .map(|o| o.layout.guests.map_or(0, |s| s.rows))
    }

    /// Paints the open dropdown.
    pub(crate) fn paint(&mut self, proxmox: &Proxmox, text: &mut TextCache) {
        if let Some(Open {
            host,
            layout,
            armed,
        }) = self.open.as_mut()
        {
            let (layout, armed) = (&*layout, armed.is_some());
            host.menu_mut()
                .paint(|cr| draw(cr, text, proxmox, layout, armed));
        }
    }

    /// Writes what the open dropdown shows, drawn again, to a PNG (the tests'
    /// `snapshot`).
    pub(crate) fn snapshot(
        &self,
        path: &str,
        proxmox: &Proxmox,
        text: &mut TextCache,
    ) -> Result<(), String> {
        let open = self.open.as_ref().ok_or("no open menu to snapshot")?;
        let (width, height) = open.host.menu().menu_size().ok_or("not a menu")?;
        let armed = open.armed.is_some();
        let image = render_image(width, height, |cr| {
            draw(cr, text, proxmox, &open.layout, armed)
        })
        .map_err(|e| e.to_string())?;
        let mut file = File::create(path).map_err(|e| e.to_string())?;
        image.write_to_png(&mut file).map_err(|e| e.to_string())
    }
}

/// `3d 4h`, `4h 12m`, `12m` or `<1m` for `secs` seconds.
pub(crate) fn format_uptime(secs: u64) -> String {
    let (days, hours, minutes) = (secs / 86_400, secs / 3600 % 24, secs / 60 % 60);
    match (days, hours, minutes) {
        (0, 0, 0) => "<1m".to_owned(),
        (0, 0, m) => format!("{m}m"),
        (0, h, m) => format!("{h}h {m}m"),
        (d, h, _) => format!("{d}d {h}h"),
    }
}

/// `9.3 GiB` or `1.8 TiB` for a byte count.
pub(crate) fn format_size(bytes: f64) -> String {
    if bytes >= TIB {
        format!("{:.1} TiB", bytes / TIB)
    } else {
        format!("{:.1} GiB", bytes / GIB)
    }
}

/// `used` of `total` as 0..=1 (0 for an unknown total).
fn fraction(used: f64, total: f64) -> f64 {
    if total > 0.0 { used / total } else { 0.0 }
}

/// The header pill's word and fill.
fn pill(proxmox: &Proxmox) -> (String, Color) {
    match proxmox.phase() {
        Phase::Unknown => ("\u{2026}".to_owned(), COLORS.overlay1),
        Phase::Off => ("off".to_owned(), COLORS.overlay1),
        Phase::Waking => ("waking".to_owned(), COLORS.yellow),
        Phase::ShuttingDown => ("shutting down".to_owned(), COLORS.peach),
        Phase::Error => ("error".to_owned(), COLORS.red),
        Phase::Up => {
            let uptime = proxmox
                .cluster
                .as_ref()
                .and_then(|c| c.nodes.first())
                .map_or(0, |n| n.uptime);
            (format!("up {}", format_uptime(uptime)), COLORS.green)
        }
    }
}

/// The whole dropdown.
fn draw(
    cr: &cairo::Context,
    text: &mut TextCache,
    proxmox: &Proxmox,
    layout: &Layout,
    armed: bool,
) -> Result<(), cairo::Error> {
    let (_word, accent) = super::label(proxmox);
    popup_chrome::draw_frame(cr, layout.size.0, layout.size.1, accent)?;
    let names = proxmox.cluster.as_ref().map_or_else(String::new, |c| {
        c.nodes
            .iter()
            .map(|n| n.name.as_str())
            .collect::<Vec<_>>()
            .join(", ")
    });
    let host = proxmox.config.as_ref().map_or("", |c| c.host());
    let summary = format!("  \u{b7}  {}", if names.is_empty() { host } else { &names });
    let (word, fill) = pill(proxmox);
    let header = Header {
        icon: Some(ICON_SERVER),
        icon_color: accent,
        title: "Proxmox",
        summary: &summary,
        pill: Some(&word),
        pill_fill: fill,
    };
    popup_chrome::draw_header(cr, text, (OUTER_MARGIN, OUTER_MARGIN, CONTENT_W), &header)?;

    if let (Some(y), Some(error)) = (layout.error_y, error_line(proxmox)) {
        let line = text.truncate(error, CONTENT_W, FONT.sm, false);
        text.draw_left(
            cr,
            (OUTER_MARGIN, y, NOTE_ROW_H),
            &line,
            FONT.sm,
            false,
            COLORS.red,
        )?;
    }
    if let Some(y) = layout.note_y {
        let note = match (
            proxmox.phase(),
            proxmox.waking_since,
            proxmox.shutdown_since,
        ) {
            (Phase::Waking, Some(since), _) => format!(
                "magic packet sent {} s ago, waiting for it to boot",
                since.elapsed().as_secs()
            ),
            (Phase::ShuttingDown, _, Some(since)) => format!(
                "shutdown sent {} s ago, stopping guests\u{2026}",
                since.elapsed().as_secs()
            ),
            (Phase::Unknown, ..) => format!("asking {host}\u{2026}"),
            _ => format!("no answer from {host}: it's off"),
        };
        let note = text.truncate(&note, CONTENT_W, FONT.sm, false);
        text.draw_left(
            cr,
            (OUTER_MARGIN, y, NOTE_ROW_H),
            &note,
            FONT.sm,
            false,
            COLORS.subtext0,
        )?;
    }
    if let Some((chip, kind)) = layout.chip {
        draw_chip(cr, text, (chip, kind), proxmox.phase(), armed)?;
    }

    let Some(cluster) = &proxmox.cluster else {
        return Ok(());
    };
    for (node, y) in cluster.nodes.iter().zip(&layout.nodes) {
        draw_node(cr, text, node, *y, accent)?;
    }
    if let Some(section) = layout.guests {
        let running = cluster.running();
        let count = format!("{running}/{} running", cluster.guests.len());
        let inner = draw_list_card(cr, text, section, ("GUESTS", &count), accent)?;
        let mut y = inner.1;
        for guest in cluster.guests.iter().take(section.rows) {
            draw_guest(cr, text, guest, (inner.0, y))?;
            y += ROW_H;
        }
        finish_list(cr, text, section, (inner.0, y), "no VMs or containers")?;
    }
    if let Some(section) = layout.storage {
        let count = cluster.storages.len().to_string();
        let inner = draw_list_card(cr, text, section, ("STORAGE", &count), accent)?;
        let mut y = inner.1;
        for storage in cluster.storages.iter().take(section.rows) {
            draw_storage(cr, text, storage, (inner.0, y))?;
            y += ROW_H;
        }
        finish_list(cr, text, section, (inner.0, y), "no storage")?;
    }
    Ok(())
}

/// The chip. Wake: green (power on) so the one thing to do while off stands
/// out from the grey frame; yellow `Send again` while waking, like the box.
/// Shut down: a plain card with red text; armed, filled red with
/// `Click again to shut down`.
fn draw_chip(
    cr: &cairo::Context,
    text: &mut TextCache,
    (rect, kind): (BoxRect, ChipKind),
    phase: Phase,
    armed: bool,
) -> Result<(), cairo::Error> {
    let (action, color) = match kind {
        ChipKind::Wake if phase == Phase::Waking => ("Send again", COLORS.yellow),
        ChipKind::Wake => ("Wake", COLORS.green),
        ChipKind::Shutdown if armed => {
            draw_rect(cr, rect, COLORS.red, COLORS.red, 0.0)?;
            let label = format!("{ICON_POWER}  Click again to shut down");
            return text.draw_centered(cr, rect, &label, FONT.md, true, COLORS.mantle);
        }
        ChipKind::Shutdown => {
            popup_chrome::draw_card(cr, rect, false, false, COLORS.red)?;
            let label = format!("{ICON_POWER}  Shut down");
            return text.draw_centered(cr, rect, &label, FONT.md, true, COLORS.red);
        }
    };
    popup_chrome::draw_card(cr, rect, true, false, color)?;
    let label = format!("{ICON_POWER}  {action}");
    text.draw_centered(cr, rect, &label, FONT.md, true, color)
}

/// The inner content's left x and width inside a card.
const fn inner() -> (f64, f64) {
    (OUTER_MARGIN + CARD_PAD, CONTENT_W - CARD_PAD * 2.0)
}

/// One node's card: name and uptime, then CPU, RAM and disk meters.
fn draw_node(
    cr: &cairo::Context,
    text: &mut TextCache,
    node: &Node,
    y: f64,
    accent: Color,
) -> Result<(), cairo::Error> {
    let rect = BoxRect {
        x: OUTER_MARGIN,
        y,
        w: CONTENT_W,
        h: node_card_h(),
    };
    popup_chrome::draw_card(cr, rect, false, false, accent)?;
    let (x, w) = inner();
    let mut ry = y + CARD_PAD;
    text.draw_left(
        cr,
        (x, ry, CARD_TITLE_H),
        &node.name,
        FONT.md,
        true,
        COLORS.text,
    )?;
    // While Proxmox stops (or starts) its status daemon, it still lists the
    // node but with every figure 0: say so instead of "0% of 0 cores".
    let has_figures = node.maxcpu > 0 && node.maxmem > 0.0;
    let (status, status_color) = if !node.online {
        ("offline".to_owned(), COLORS.red)
    } else if has_figures {
        (
            format!("up {}", format_uptime(node.uptime)),
            COLORS.overlay1,
        )
    } else {
        ("no figures right now".to_owned(), COLORS.overlay1)
    };
    text.draw_right(
        cr,
        (x + w, ry, CARD_TITLE_H),
        &status,
        FONT.sm,
        false,
        status_color,
    )?;
    ry += CARD_TITLE_H;
    let cores = node.maxcpu;
    // `--` for a figure Proxmox didn't give (its total is 0); the meter
    // stays empty then too.
    let used_of = |used: f64, total: f64| {
        if total > 0.0 {
            format!("{} / {}", format_size(used), format_size(total))
        } else {
            "--".to_owned()
        }
    };
    let rows = [
        (
            "CPU",
            node.cpu,
            if cores > 0 {
                format!("{:.0}% of {cores} cores", node.cpu * 100.0)
            } else {
                "--".to_owned()
            },
        ),
        (
            "RAM",
            fraction(node.mem, node.maxmem),
            used_of(node.mem, node.maxmem),
        ),
        (
            "Disk",
            fraction(node.disk, node.maxdisk),
            used_of(node.disk, node.maxdisk),
        ),
    ];
    for (label, fill, value) in rows {
        text.draw_left(cr, (x, ry, ROW_H), label, FONT.sm, false, COLORS.subtext0)?;
        let meter = BoxRect {
            x: x + LABEL_W,
            y: ry + (ROW_H - METER_H) / 2.0,
            w: METER_W,
            h: METER_H,
        };
        meter_bar::draw(cr, meter, fill, METER_SEGMENTS, &HEAT, COLORS.surface0)?;
        text.draw_right(cr, (x + w, ry, ROW_H), &value, FONT.sm, false, COLORS.text)?;
        ry += ROW_H;
    }
    Ok(())
}

/// A list card's frame and title row (`title` left, `count` right). Returns
/// the inner left x and the first row's top.
fn draw_list_card(
    cr: &cairo::Context,
    text: &mut TextCache,
    section: Section,
    (title, count): (&str, &str),
    accent: Color,
) -> Result<(f64, f64), cairo::Error> {
    let rect = BoxRect {
        x: OUTER_MARGIN,
        y: section.y,
        w: CONTENT_W,
        h: section.height(),
    };
    popup_chrome::draw_card(cr, rect, false, false, accent)?;
    let (x, w) = inner();
    let y = section.y + CARD_PAD;
    text.draw_left(
        cr,
        (x, y, CARD_TITLE_H),
        title,
        FONT.xs,
        true,
        COLORS.overlay0,
    )?;
    text.draw_right(
        cr,
        (x + w, y, CARD_TITLE_H),
        count,
        FONT.xs,
        true,
        COLORS.overlay0,
    )?;
    Ok((x, y + CARD_TITLE_H))
}

/// After a list's rows: `+N more`, or `empty` for a list with no rows.
fn finish_list(
    cr: &cairo::Context,
    text: &mut TextCache,
    section: Section,
    (x, y): (f64, f64),
    empty: &str,
) -> Result<(), cairo::Error> {
    let line = if section.more > 0 {
        format!("+{} more", section.more)
    } else if section.rows == 0 {
        empty.to_owned()
    } else {
        return Ok(());
    };
    text.draw_left(cr, (x, y, ROW_H), &line, FONT.sm, false, COLORS.overlay1)
}

/// One guest's row: state dot, `VM`/`CT`, id, name; CPU and RAM while
/// running, else its state.
fn draw_guest(
    cr: &cairo::Context,
    text: &mut TextCache,
    guest: &Guest,
    (x, y): (f64, f64),
) -> Result<(), cairo::Error> {
    let (_, w) = inner();
    let (dot_color, state_text) = match guest.state {
        GuestState::Running => (COLORS.green, None),
        GuestState::Paused => (COLORS.yellow, Some(("paused", COLORS.yellow))),
        GuestState::Stopped => (COLORS.overlay0, Some(("stopped", COLORS.overlay0))),
    };
    // A filled circle.
    text.draw_left(cr, (x, y, ROW_H), "\u{25cf}", FONT.sm, false, dot_color)?;
    let tag = match guest.kind {
        GuestKind::Vm => "VM",
        GuestKind::Ct => "CT",
    };
    text.draw_left(
        cr,
        (x + TAG_X, y, ROW_H),
        tag,
        FONT.xs,
        true,
        COLORS.overlay0,
    )?;
    text.draw_left(
        cr,
        (x + ID_X, y, ROW_H),
        &guest.vmid.to_string(),
        FONT.sm,
        false,
        COLORS.overlay1,
    )?;
    let right = match state_text {
        Some((word, color)) => (word.to_owned(), color),
        None => (
            format!(
                "{:.0}%  {} / {}",
                guest.cpu * 100.0,
                format_size(guest.mem),
                format_size(guest.maxmem)
            ),
            COLORS.subtext0,
        ),
    };
    let right_w = text.width(&right.0, FONT.sm, false);
    text.draw_right(cr, (x + w, y, ROW_H), &right.0, FONT.sm, false, right.1)?;
    let name_w = w - NAME_X - right_w - COL_GAP;
    let name = text.truncate(&guest.name, name_w, FONT.md, false);
    let name_color = if guest.state == GuestState::Stopped {
        COLORS.subtext0
    } else {
        COLORS.text
    };
    text.draw_left(
        cr,
        (x + NAME_X, y, ROW_H),
        &name,
        FONT.md,
        false,
        name_color,
    )
}

/// One storage's row: its name, used / total and how full.
fn draw_storage(
    cr: &cairo::Context,
    text: &mut TextCache,
    storage: &Storage,
    (x, y): (f64, f64),
) -> Result<(), cairo::Error> {
    let (_, w) = inner();
    let (right, color) = if storage.available {
        (
            format!(
                "{} / {}  \u{b7}  {:.0}%",
                format_size(storage.disk),
                format_size(storage.maxdisk),
                fraction(storage.disk, storage.maxdisk) * 100.0
            ),
            COLORS.subtext0,
        )
    } else {
        ("unavailable".to_owned(), COLORS.red)
    };
    let right_w = text.width(&right, FONT.sm, false);
    text.draw_right(cr, (x + w, y, ROW_H), &right, FONT.sm, false, color)?;
    let name = text.truncate(&storage.name, w - right_w - COL_GAP, FONT.md, false);
    text.draw_left(cr, (x, y, ROW_H), &name, FONT.md, false, COLORS.text)
}

#[cfg(test)]
mod tests {
    use super::{format_size, format_uptime};

    #[test]
    fn formats_uptime_and_sizes() {
        assert_eq!(format_uptime(30), "<1m");
        assert_eq!(format_uptime(720), "12m");
        assert_eq!(format_uptime(4 * 3600 + 720), "4h 12m");
        assert_eq!(format_uptime(3 * 86_400 + 4 * 3600 + 59), "3d 4h");
        assert_eq!(format_size(1024.0 * 1024.0 * 1024.0 * 1.5), "1.5 GiB");
        assert_eq!(format_size(2.0 * 1024.0_f64.powi(4)), "2.0 TiB");
    }
}
