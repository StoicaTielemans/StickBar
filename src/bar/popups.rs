//! Every kind of dropdown, in one place (the C bar's `POPUP_ROUTES` table in
//! `wayland/pointer.c`). Each kind is a field. Routing (pointer events,
//! configures, the tests' `menu` command) asks each field in turn whether a
//! surface is one of its own. Each kind is independent: opening one doesn't
//! close another, as in the C bar (`docs/popups.md`, "Popups mostly don't
//! coordinate").

use smithay_client_toolkit::{reexports::client::protocol::wl_surface::WlSurface, shm::Shm};

use super::{popup_host::PopupHost, popup_surface::PopupSurface};
use crate::{
    modules::{clock::calendar::CalendarPopup, mem::popup::MemPopup, powermenu::PowerMenu},
    render::Renderer,
};

/// All dropdowns. New kinds are added as their modules are ported.
#[derive(Default)]
pub(crate) struct Popups {
    /// The clock's year calendar.
    pub(crate) calendar: CalendarPopup,
    /// The power menu (Arch logo / SIGUSR1).
    pub(crate) power: PowerMenu,
    /// The memory dropdown.
    pub(crate) mem: MemPopup,
}

impl Popups {
    /// Routes a configure to the popup surface `surface`, if it is one, and
    /// repaints its owner if the configure asks for it. Returns false if no
    /// popup owns it.
    pub(crate) fn configure(
        &mut self,
        surface: &WlSurface,
        shm: &Shm,
        (width, height): (u32, u32),
        renderer: &mut Renderer,
    ) -> bool {
        let text = &mut renderer.text;
        if let Some(overlay) = self.power.surface_mut(surface) {
            if overlay.configure(shm, width, height) {
                self.power.paint_all(text);
            }
            return true;
        }
        if let Some(popup) = self.mem.host_mut().and_then(|h| h.surface_mut(surface)) {
            if popup.configure(shm, width, height) {
                self.mem.paint(&renderer.sysmon, text);
            }
            return true;
        }
        if let Some(popup) = self
            .calendar
            .host_mut()
            .and_then(|h| h.surface_mut(surface))
        {
            if popup.configure(shm, width, height) {
                self.calendar.paint(text);
            }
            return true;
        }
        false
    }

    /// The compositor closed popup surface `surface`.
    pub(crate) fn closed(&mut self, surface: &WlSurface) {
        let hosts = [self.calendar.host_mut(), self.mem.host_mut()];
        let popup = self.power.surface_mut(surface).or_else(|| {
            hosts
                .into_iter()
                .flatten()
                .find_map(|h| h.surface_mut(surface))
        });
        if let Some(popup) = popup {
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
            .or_else(|| self.mem.menu())
            .or_else(|| self.calendar.menu())
    }

    /// Every open catcher, across all dropdowns.
    pub(crate) fn catchers(&self) -> impl Iterator<Item = &PopupSurface> {
        [self.calendar.host(), self.mem.host()]
            .into_iter()
            .flatten()
            .flat_map(PopupHost::catchers)
    }

    /// Saves the latest open menu's picture as a PNG at `path`.
    pub(crate) fn snapshot(&mut self, path: &str, renderer: &mut Renderer) -> Result<(), String> {
        if self.power.is_open() {
            return self.power.snapshot(path, &mut renderer.text);
        }
        if self.mem.host().is_some() {
            return self
                .mem
                .snapshot(path, &renderer.sysmon, &mut renderer.text);
        }
        self.calendar.snapshot(path, &mut renderer.text)
    }
}
