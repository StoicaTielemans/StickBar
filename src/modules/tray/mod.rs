//! The system tray (the C bar's `modules/tray/tray.c`): one icon per
//! `StatusNotifierItem` at the bar's right edge, right of the clock. The items
//! come from the `dbus` thread (`src/dbus/`) as [`TrayEvent`]s; this side
//! keeps them in registration order, turns their icons into cairo surfaces,
//! and draws them. A click on an icon opens its menu (`menu.rs`): any button
//! if the item has a menu, else a left click activates it (the reference
//! bar's rule, `docs/modules/tray.md`).

pub(crate) mod menu;

use std::io::Cursor;

use crate::{
    config::{BAR, COLORS, Color},
    dbus::{Icon, TrayEvent},
    render::{Action, BoxRect, ClickTarget, module_box::draw_rect},
};

/// One tray item, as the bar draws it.
#[derive(Debug)]
pub(crate) struct TrayItem {
    /// The `dbus` thread's id for it.
    pub(crate) id: u64,
    /// SNI `Title`, or its `Id` (the menu header, and the tests' handle on it).
    pub(crate) title: String,
    /// Any click opens its menu (else a left click activates it).
    pub(crate) has_menu: bool,
    /// Its icon at the bar's size, or `None` for the placeholder square.
    pub(crate) icon: Option<cairo::ImageSurface>,
}

/// Every tray item, in registration order.
#[derive(Debug, Default)]
pub(crate) struct Tray {
    /// The items.
    items: Vec<TrayItem>,
}

impl Tray {
    /// Applies an item change from the `dbus` thread. Returns whether the bar
    /// changed (menu answers are the caller's business and return false).
    pub(crate) fn apply(&mut self, event: TrayEvent) -> bool {
        match event {
            TrayEvent::Added {
                id,
                title,
                has_menu,
                icon,
            } => {
                self.items.push(TrayItem {
                    id,
                    title,
                    has_menu,
                    icon: icon.and_then(icon_surface),
                });
                true
            }
            TrayEvent::Icon { id, icon } => {
                let Some(item) = self.items.iter_mut().find(|i| i.id == id) else {
                    return false;
                };
                match icon_surface(icon) {
                    Some(surface) => {
                        item.icon = Some(surface);
                        true
                    }
                    None => false,
                }
            }
            TrayEvent::Removed { id } => {
                let before = self.items.len();
                self.items.retain(|i| i.id != id);
                self.items.len() != before
            }
            TrayEvent::Menu { .. } => false,
        }
    }

    /// The item with id `id`.
    pub(crate) fn item(&self, id: u64) -> Option<&TrayItem> {
        self.items.iter().find(|i| i.id == id)
    }
}

/// The bar's height in px, which is also every tray icon's size.
fn icon_size() -> i32 {
    i32::from(BAR.height)
}

/// Turns the `dbus` thread's picture into a cairo surface at the icon size
/// (the C bar's `sni_icon_convert` / `load_and_scale_png`): whatever sizes an
/// app ships, every tray icon ends up the same size. `None` if it can't be
/// read, which draws the placeholder.
fn icon_surface(icon: Icon) -> Option<cairo::ImageSurface> {
    let source = match icon {
        Icon::Argb(argb) => {
            let stride = argb.width.checked_mul(4)?;
            cairo::ImageSurface::create_for_data(
                argb.data,
                cairo::Format::ARgb32,
                argb.width,
                argb.height,
                stride,
            )
            .ok()?
        }
        Icon::Png(bytes) => cairo::ImageSurface::create_from_png(&mut Cursor::new(bytes)).ok()?,
    };
    let (w, h) = (source.width(), source.height());
    if w <= 0 || h <= 0 {
        return None;
    }
    let size = icon_size();
    let dest = cairo::ImageSurface::create(cairo::Format::ARgb32, size, size).ok()?;
    let cr = cairo::Context::new(&dest).ok()?;
    cr.scale(
        f64::from(size) / f64::from(w),
        f64::from(size) / f64::from(h),
    );
    cr.set_source_surface(&source, 0.0, 0.0).ok()?;
    cr.source().set_filter(cairo::Filter::Good);
    cr.paint().ok()?;
    drop(cr);
    dest.flush();
    Some(dest)
}

/// Draws the icons right to left, ending at `right_x`, in the row
/// `top..top + height`. Returns the region's box (`None` when there are no
/// items: the tray then takes no space, like the reference's
/// `visible: count > 0`) and a click target per icon.
pub(crate) fn render(
    cr: &cairo::Context,
    tray: &Tray,
    right_x: f64,
    (top, height): (f64, f64),
) -> Result<(Option<BoxRect>, Vec<ClickTarget>), cairo::Error> {
    let Some(count) = u32::try_from(tray.items.len()).ok().filter(|&n| n > 0) else {
        return Ok((None, Vec::new()));
    };
    let size = f64::from(icon_size());
    let total = f64::from(count) * size + f64::from(count - 1) * BAR.item_spacing;
    let start_x = right_x - total;
    let icon_y = top + (height - size) / 2.0;
    let mut targets = Vec::with_capacity(tray.items.len());
    let mut x = start_x;
    for item in &tray.items {
        let rect = BoxRect {
            x,
            y: icon_y,
            w: size,
            h: size,
        };
        draw_icon(cr, item.icon.as_ref(), rect)?;
        targets.push(ClickTarget {
            rect,
            action: Action::TrayIcon(item.id),
        });
        x += size + BAR.item_spacing;
    }
    let region = BoxRect {
        x: start_x,
        y: top,
        w: right_x - start_x,
        h: height,
    };
    Ok((Some(region), targets))
}

/// An icon (already at the right size: a plain blit) at `rect`'s corner, or
/// a muted placeholder square when there's none. The reference bar has no
/// "missing icon" look; a square beats a blank gap.
pub(crate) fn draw_icon(
    cr: &cairo::Context,
    icon: Option<&cairo::ImageSurface>,
    rect: BoxRect,
) -> Result<(), cairo::Error> {
    match icon {
        Some(icon) => {
            cr.save()?;
            cr.set_source_surface(icon, rect.x, rect.y)?;
            cr.paint()?;
            cr.restore()
        }
        None => draw_rect(cr, rect, COLORS.overlay0, Color::TRANSPARENT, 0.0),
    }
}
