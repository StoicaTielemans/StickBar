//! Every kind of dropdown, in one place (the C bar's `POPUP_ROUTES` table in
//! `wayland/pointer.c`). Each kind is a field. Routing (pointer events,
//! configures, the tests' `menu` command) asks each field in turn whether a
//! surface is one of its own. Each kind is independent: opening one doesn't
//! close another, as in the C bar (`docs/popups.md`, "Popups mostly don't
//! coordinate").

use smithay_client_toolkit::{reexports::client::protocol::wl_surface::WlSurface, shm::Shm};

use super::{popup_host::PopupHost, popup_surface::PopupSurface};
use crate::{
    modules::{clock::calendar::CalendarPopup, powermenu::PowerMenu},
    render::text::TextCache,
};

/// All dropdowns. New kinds are added as their modules are ported.
#[derive(Default)]
pub(crate) struct Popups {
    /// The clock's year calendar.
    pub(crate) calendar: CalendarPopup,
    /// The power menu (Arch logo / SIGUSR1).
    pub(crate) power: PowerMenu,
}

impl Popups {
    /// Routes a configure to the popup surface `surface`, if it is one.
    /// Returns false if no popup owns it.
    pub(crate) fn configure(
        &mut self,
        surface: &WlSurface,
        shm: &Shm,
        (width, height): (u32, u32),
        text: &mut TextCache,
    ) -> bool {
        if let Some(overlay) = self.power.surface_mut(surface) {
            if overlay.configure(shm, width, height) {
                self.power.paint_all(text);
            }
            return true;
        }
        let Some(popup) = self
            .calendar
            .host_mut()
            .and_then(|h| h.surface_mut(surface))
        else {
            return false;
        };
        if popup.configure(shm, width, height) {
            self.calendar.paint(text);
        }
        true
    }

    /// The compositor closed popup surface `surface`.
    pub(crate) fn closed(&mut self, surface: &WlSurface) {
        if let Some(overlay) = self.power.surface_mut(surface) {
            overlay.closed();
        }
        if let Some(popup) = self
            .calendar
            .host_mut()
            .and_then(|h| h.surface_mut(surface))
        {
            popup.closed();
        }
    }

    /// The menu the tests' `menu`, `menu-*` and `snapshot` commands act on:
    /// the power menu's first overlay when it's open (it covers everything),
    /// otherwise the open dropdown.
    pub(crate) fn latest_menu(&self) -> Option<&PopupSurface> {
        self.power
            .surfaces()
            .first()
            .or_else(|| self.calendar.menu())
    }

    /// Every open catcher, across all dropdowns.
    pub(crate) fn catchers(&self) -> impl Iterator<Item = &PopupSurface> {
        self.calendar
            .host()
            .into_iter()
            .flat_map(PopupHost::catchers)
    }

    /// Saves the latest open menu's picture as a PNG at `path`.
    pub(crate) fn snapshot(&mut self, path: &str, text: &mut TextCache) -> Result<(), String> {
        if self.power.is_open() {
            return self.power.snapshot(path, text);
        }
        self.calendar.snapshot(path, text)
    }
}
