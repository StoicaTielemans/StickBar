//! Every kind of dropdown, in one place (the C bar's `POPUP_ROUTES` table in
//! `wayland/pointer.c`). Each kind is a field. Routing (pointer events,
//! configures, the tests' `menu` command) asks each field in turn whether a
//! surface is one of its own. Mostly each kind is independent: opening one
//! doesn't close another, as in the C bar (`docs/popups.md`, "Popups mostly
//! don't coordinate"). The exception is the CPU, GPU and memory dropdowns,
//! which open at the same spot and so replace each other.

use smithay_client_toolkit::{reexports::client::protocol::wl_surface::WlSurface, shm::Shm};

use super::{popup_host::PopupHost, popup_surface::PopupSurface};
use crate::{
    modules::{
        clock::calendar::CalendarPopup, cpu::popup::CpuPopup, gpu::popup::GpuPopup,
        mem::popup::MemPopup, powermenu::PowerMenu,
    },
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
    /// The CPU dropdown.
    pub(crate) cpu: CpuPopup,
    /// The GPU dropdown.
    pub(crate) gpu: GpuPopup,
}

impl Popups {
    /// The open hosts of the dropdowns that have one (everything but the
    /// power menu, whose overlays are their own catchers).
    fn hosts(&self) -> [Option<&PopupHost>; 4] {
        [
            self.calendar.host(),
            self.mem.host(),
            self.cpu.host(),
            self.gpu.host(),
        ]
    }

    /// Whether `surface` belongs to one of the sysmon dropdowns: `Some(true)`
    /// for a menu, `Some(false)` for a catcher, `None` otherwise.
    pub(crate) fn sysmon_surface(&self, surface: &WlSurface) -> Option<bool> {
        [self.cpu.host(), self.gpu.host(), self.mem.host()]
            .into_iter()
            .flatten()
            .find_map(|host| {
                if host.is_menu(surface) {
                    Some(true)
                } else {
                    host.is_catcher(surface).then_some(false)
                }
            })
    }

    /// Closes every sysmon dropdown (before opening one: they share a spot).
    pub(crate) fn close_sysmon(&mut self) {
        self.cpu.close();
        self.gpu.close();
        self.mem.close();
    }

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
        if let Some(popup) = self.cpu.host_mut().and_then(|h| h.surface_mut(surface)) {
            if popup.configure(shm, width, height) {
                self.cpu.paint(&renderer.sysmon, text);
            }
            return true;
        }
        if let Some(popup) = self.gpu.host_mut().and_then(|h| h.surface_mut(surface)) {
            if popup.configure(shm, width, height) {
                self.gpu.paint(&renderer.sysmon, text);
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
        let hosts = [
            self.calendar.host_mut(),
            self.mem.host_mut(),
            self.cpu.host_mut(),
            self.gpu.host_mut(),
        ];
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
            .or_else(|| self.cpu.menu())
            .or_else(|| self.gpu.menu())
            .or_else(|| self.mem.menu())
            .or_else(|| self.calendar.menu())
    }

    /// Every open catcher, across all dropdowns.
    pub(crate) fn catchers(&self) -> impl Iterator<Item = &PopupSurface> {
        self.hosts()
            .into_iter()
            .flatten()
            .flat_map(PopupHost::catchers)
    }

    /// Saves the latest open menu's picture as a PNG at `path`.
    pub(crate) fn snapshot(&mut self, path: &str, renderer: &mut Renderer) -> Result<(), String> {
        let (sysmon, text) = (&renderer.sysmon, &mut renderer.text);
        if self.power.is_open() {
            self.power.snapshot(path, text)
        } else if self.cpu.host().is_some() {
            self.cpu.snapshot(path, sysmon, text)
        } else if self.gpu.host().is_some() {
            self.gpu.snapshot(path, sysmon, text)
        } else if self.mem.host().is_some() {
            self.mem.snapshot(path, sysmon, text)
        } else {
            self.calendar.snapshot(path, text)
        }
    }
}
