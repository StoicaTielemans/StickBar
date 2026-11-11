//! What every dropdown shares around its content (the C bar's
//! `bar/popup_host.c`): the menu surface placed under the box that opened it,
//! plus one full-monitor click-catcher per monitor, so a click anywhere else
//! closes it. A dropdown holds an `Option<PopupHost>`: `Some` while open,
//! and dropping it closes everything.
//!
//! Right-region boxes open leftwards from their right edge (`open_right`),
//! left-region ones rightwards from their left edge (`open_left`); the wrong
//! one pushes the dropdown off-screen (BUG.md, "CPU/GPU/memory dropdowns
//! off-screen"). Resizing while open arrives with the first dropdown whose
//! height changes (the mixer, M9).

use smithay_client_toolkit::{
    reexports::client::protocol::{wl_output::WlOutput, wl_surface::WlSurface},
    shell::wlr_layer::Anchor,
};

use super::popup_surface::{PopupEnv, PopupSurface};
use crate::{
    config::BAR,
    render::{BoxRect, whole_px},
};

/// An open dropdown: its menu and catchers.
pub(crate) struct PopupHost {
    /// The dropdown itself.
    menu: PopupSurface,
    /// One transparent click-catcher per tracked monitor. All of them, not just
    /// the menu's: with a catcher on one monitor only, a click on another would
    /// leave the menu stuck open.
    catchers: Vec<PopupSurface>,
}

impl PopupHost {
    /// Opens a `width` x `height` dropdown on `output` (`output_width` px wide)
    /// under the box `trigger`, growing leftwards from the box's right edge, with
    /// a catcher on each of `all_outputs`.
    pub(crate) fn open_right<'o>(
        env: &PopupEnv<'_>,
        (output, output_width): (&WlOutput, i32),
        trigger: BoxRect,
        size: (i32, i32),
        all_outputs: impl Iterator<Item = &'o WlOutput>,
    ) -> Self {
        // The margin is measured from the edge of the bar's own reserved
        // strip, not the monitor edge, so the bar's height comes off (a box
        // spanning the bar gives 0). Hyprland behaviour, found the hard way in
        // the C bar: without it the first popup sat a full bar-height too low.
        let top = whole_px(trigger.y + trigger.h) - i32::from(BAR.height);
        let right = (output_width - whole_px(trigger.x + trigger.w)).max(0);
        let menu = PopupSurface::menu(
            env,
            output,
            size,
            (top, right, 0, 0),
            Anchor::TOP | Anchor::RIGHT,
        );
        let catchers = all_outputs.map(|o| PopupSurface::catcher(env, o)).collect();
        Self { menu, catchers }
    }

    /// Opens a `width` x `height` dropdown on `output` under the box
    /// `trigger`, growing rightwards from the box's left edge (left-region
    /// boxes), with a catcher on each of `all_outputs`.
    pub(crate) fn open_left<'o>(
        env: &PopupEnv<'_>,
        output: &WlOutput,
        trigger: BoxRect,
        size: (i32, i32),
        all_outputs: impl Iterator<Item = &'o WlOutput>,
    ) -> Self {
        // The same bar-height correction as `open_right`.
        let top = whole_px(trigger.y + trigger.h) - i32::from(BAR.height);
        let left = whole_px(trigger.x);
        let menu = PopupSurface::menu(
            env,
            output,
            size,
            (top, 0, 0, left),
            Anchor::TOP | Anchor::LEFT,
        );
        let catchers = all_outputs.map(|o| PopupSurface::catcher(env, o)).collect();
        Self { menu, catchers }
    }

    /// The menu surface.
    pub(crate) const fn menu(&self) -> &PopupSurface {
        &self.menu
    }

    /// The menu surface, for painting.
    pub(crate) const fn menu_mut(&mut self) -> &mut PopupSurface {
        &mut self.menu
    }

    /// The catchers.
    pub(crate) fn catchers(&self) -> &[PopupSurface] {
        &self.catchers
    }

    /// Whether `surface` is this dropdown's menu.
    pub(crate) fn is_menu(&self, surface: &WlSurface) -> bool {
        self.menu.wl_surface() == surface
    }

    /// Whether `surface` is one of this dropdown's catchers.
    pub(crate) fn is_catcher(&self, surface: &WlSurface) -> bool {
        self.catchers.iter().any(|c| c.wl_surface() == surface)
    }

    /// The menu or catcher whose surface is `surface`, if any.
    pub(crate) fn surface_mut(&mut self, surface: &WlSurface) -> Option<&mut PopupSurface> {
        std::iter::once(&mut self.menu)
            .chain(self.catchers.iter_mut())
            .find(|s| s.wl_surface() == surface)
    }
}
