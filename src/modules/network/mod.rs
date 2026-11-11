//! The network box, e.g. `󰈀 ↓12.3 KB/s ↑4.5 KB/s` in teal (the C bar's
//! `modules/network/network.c`). Teal, not the reference's green: the GPU box
//! already has green. A right click opens its dropdown (`popup.rs`).

pub(crate) mod popup;

use crate::{
    config::{
        COLORS,
        glyphs::{
            ARROW_DOWN, ARROW_UP, ICON_ETHERNET, ICON_ETHERNET_OFF, ICON_NETWORK_UNKNOWN,
            ICON_WIFI, ICON_WIFI_OFF,
        },
    },
    network::{Network, format_rate},
    render::{
        BoxRect,
        module_box::{self, Anchor},
        text::TextCache,
    },
};

/// The status icon, as in the reference's `icon()`: no default route at
/// all, then down/up each split on Wi-Fi vs Ethernet.
pub(crate) fn status_glyph(network: &Network) -> char {
    match (network.iface.is_empty(), network.connected, network.wifi) {
        (true, _, _) => ICON_NETWORK_UNKNOWN,
        (false, false, true) => ICON_WIFI_OFF,
        (false, false, false) => ICON_ETHERNET_OFF,
        (false, true, true) => ICON_WIFI,
        (false, true, false) => ICON_ETHERNET,
    }
}

/// Draws the box ending at `right_x` (right region), spanning `top..top + height`.
pub(crate) fn render(
    cr: &cairo::Context,
    text: &mut TextCache,
    network: &Network,
    right_x: f64,
    (top, height): (f64, f64),
) -> Result<BoxRect, cairo::Error> {
    let icon = status_glyph(network);
    let label = if network.connected {
        let (rx, tx) = (format_rate(network.rx_rate), format_rate(network.tx_rate));
        format!("{icon} {ARROW_DOWN}{rx} {ARROW_UP}{tx}")
    } else {
        format!("{icon} Disconnected")
    };
    module_box::label(
        cr,
        text,
        (Anchor::FromRight, right_x),
        (top, height),
        &label,
        COLORS.teal,
    )
}
