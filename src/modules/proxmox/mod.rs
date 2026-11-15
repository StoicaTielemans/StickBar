//! The Proxmox box (`docs/modules/proxmox.md`; the data is `src/proxmox/`):
//! the server icon and how many guests run, `<server> 3/5`, in green while
//! the server is up; grey `off` while nothing answers; yellow `...` while
//! waking; the same `3/5` in peach while shutting down (it counts down as
//! the guests stop; `...` before any count is known); red `!` while it
//! answers with an error; grey `--` before the first poll. Not shown at all without a settings file. A right click opens the
//! dropdown (`popup.rs`), which has the Wake chip; a left click does nothing,
//! so a stray click never boots the server (the user's choice).

pub(crate) mod popup;

use crate::{
    config::{COLORS, Color, glyphs::ICON_SERVER},
    proxmox::{Phase, Proxmox},
    render::{
        BoxRect,
        module_box::{self, Anchor},
        text::TextCache,
    },
};

/// The box's text after the icon, and its colour, for what the server is doing.
pub(crate) fn label(proxmox: &Proxmox) -> (String, Color) {
    match proxmox.phase() {
        Phase::Unknown => ("--".to_owned(), COLORS.overlay1),
        Phase::Off => ("off".to_owned(), COLORS.overlay1),
        // An ellipsis: "on its way".
        Phase::Waking => ("\u{2026}".to_owned(), COLORS.yellow),
        // The guest count, in peach: it falls as Proxmox stops them, which
        // shows the shutdown's progress in the box's usual shape.
        Phase::ShuttingDown => (
            guest_count(proxmox).unwrap_or_else(|| "\u{2026}".to_owned()),
            COLORS.peach,
        ),
        Phase::Error => ("!".to_owned(), COLORS.red),
        Phase::Up => (
            guest_count(proxmox).unwrap_or_else(|| "0/0".to_owned()),
            COLORS.green,
        ),
    }
}

/// `running/total` guests from the last answer, if there is one.
fn guest_count(proxmox: &Proxmox) -> Option<String> {
    proxmox
        .cluster
        .as_ref()
        .map(|c| format!("{}/{}", c.running(), c.guests.len()))
}

/// Draws the box starting at `left_x` (left region, after the memory box),
/// spanning `top..top + height`; `None` (no box, no space) without settings.
pub(crate) fn render(
    cr: &cairo::Context,
    text: &mut TextCache,
    proxmox: &Proxmox,
    left_x: f64,
    (top, height): (f64, f64),
) -> Result<Option<BoxRect>, cairo::Error> {
    if !proxmox.enabled() {
        return Ok(None);
    }
    let (word, color) = label(proxmox);
    module_box::label(
        cr,
        text,
        (Anchor::FromLeft, left_x),
        (top, height),
        &format!("{ICON_SERVER} {word}"),
        color,
    )
    .map(Some)
}
