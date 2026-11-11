//! The mouse (the C bar's `wayland/pointer.c`): getting a pointer from the
//! seat, and routing its events to whatever is under it.
//!
//! **One routing path:** real pointer events and the tests' injected ones
//! (`click`, `menu-click`, `menu-motion`, `catcher-click`) all go through
//! `App::pointer_button` / `App::pointer_motion` with a surface and a
//! surface-local position. Only the compositor's "which surface is under the
//! cursor" step is skipped by the tests.
//!
//! Routing order, as in the C bar: a dropdown's own menu surface gets the
//! event; a dropdown's catcher closes that dropdown; otherwise it's a bar
//! surface, and the module whose box is under the pointer handles it. A popup
//! or catcher surface is never a bar surface, so checking popups first is
//! required, not just tidier.
//!
//! Not ported yet, waiting for their first users: button releases and scrolling
//! (the volume/mixer sliders, M9).

use smithay_client_toolkit::{
    reexports::client::{
        Connection, QueueHandle,
        protocol::{wl_pointer::WlPointer, wl_seat::WlSeat, wl_surface::WlSurface},
    },
    seat::{
        Capability, SeatHandler, SeatState,
        pointer::{PointerEvent, PointerEventKind, PointerHandler},
    },
};

use super::{App, Output};
use crate::{bar::popup_surface::PopupEnv, render::BoxRect};

/// Linux input event codes for the mouse buttons (`linux/input-event-codes.h`).
pub(crate) const BTN_LEFT: u32 = 0x110;
/// Right button.
pub(crate) const BTN_RIGHT: u32 = 0x111;
/// Middle button (wheel click).
pub(crate) const BTN_MIDDLE: u32 = 0x112;

impl App {
    /// A button was pressed with the pointer at (x, y) on `surface`.
    pub(crate) fn pointer_button(&mut self, surface: &WlSurface, x: f64, y: f64, button: u32) {
        // 1. The calendar's own menu absorbs the click; its catcher closes it.
        if let Some(host) = self.popups.calendar.host() {
            if host.is_menu(surface) {
                self.popups
                    .calendar
                    .click(x, y, button, &mut self.renderer.text);
                return;
            }
            if host.is_catcher(surface) {
                self.popups.calendar.close();
                return;
            }
        }

        // 2. A bar: the module whose box is under the pointer.
        let Some(position) = self.outputs.iter().position(|o| {
            o.bar
                .as_ref()
                .is_some_and(|bar| bar.wl_surface() == surface)
        }) else {
            return;
        };
        let clock = self
            .outputs
            .get(position)
            .and_then(|o| o.bar.as_ref())
            .and_then(|bar| {
                bar.boxes()
                    .iter()
                    .find(|b| b.name == "clock" && b.rect.contains(x, y))
            })
            .map(|b| b.rect);
        // Right-click on the clock toggles the year calendar. Left does nothing:
        // the clock has no obvious primary action (docs/popups.md).
        if button == BTN_RIGHT
            && let Some(rect) = clock
        {
            self.toggle_calendar(position, rect);
        }
    }

    /// The pointer moved to (x, y) on `surface`: hover highlights.
    pub(crate) fn pointer_motion(&mut self, surface: &WlSurface, x: f64, y: f64) {
        if self
            .popups
            .calendar
            .host()
            .is_some_and(|h| h.is_menu(surface))
        {
            self.popups.calendar.motion(x, y, &mut self.renderer.text);
        }
    }

    /// The pointer left `surface`: drop any hover highlight there.
    fn pointer_leave(&mut self, surface: &WlSurface) {
        // A point outside the popup matches no target, clearing the hover.
        self.pointer_motion(surface, -1.0, -1.0);
    }

    /// Opens (or closes) the calendar under the clock box `trigger` on the
    /// monitor at `position` in `outputs`.
    fn toggle_calendar(&mut self, position: usize, trigger: BoxRect) {
        let Some(output_width) = self.outputs.get(position).map(|o| self.output_size(o).0) else {
            return;
        };
        // Destructured so each piece can be borrowed separately.
        let Self {
            qh,
            compositor,
            layer_shell,
            viewporter,
            test_mode,
            outputs,
            popups,
            renderer,
            ..
        } = self;
        let Some(output) = outputs.get(position) else {
            return;
        };
        let env = PopupEnv {
            qh,
            compositor,
            layer_shell,
            viewporter: viewporter.as_ref(),
            test_mode: *test_mode,
        };
        popups.calendar.toggle(
            &env,
            (output.wl_output(), output_width),
            trigger,
            outputs.iter().map(Output::wl_output),
            &mut renderer.text,
        );
    }
}

impl SeatHandler for App {
    fn seat_state(&mut self) -> &mut SeatState {
        &mut self.seat_state
    }

    fn new_seat(&mut self, _: &Connection, _: &QueueHandle<Self>, _: WlSeat) {}

    /// The seat gained a device: if it's a mouse, start listening to it.
    fn new_capability(
        &mut self,
        _conn: &Connection,
        qh: &QueueHandle<Self>,
        seat: WlSeat,
        capability: Capability,
    ) {
        if capability == Capability::Pointer && self.pointer.is_none() {
            match self.seat_state.get_pointer(qh, &seat) {
                Ok(pointer) => self.pointer = Some(pointer),
                Err(err) => eprintln!("stickBar: couldn't get the pointer: {err}"),
            }
        }
    }

    /// The seat lost a device: if it was the mouse, let go of it.
    fn remove_capability(
        &mut self,
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
        _seat: WlSeat,
        capability: Capability,
    ) {
        if capability == Capability::Pointer
            && let Some(pointer) = self.pointer.take()
        {
            pointer.release();
        }
    }

    fn remove_seat(&mut self, _: &Connection, _: &QueueHandle<Self>, _: WlSeat) {}
}

impl PointerHandler for App {
    /// SCTK groups pointer events into frames; each event names its surface and
    /// carries the surface-local position.
    fn pointer_frame(
        &mut self,
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
        _pointer: &WlPointer,
        events: &[PointerEvent],
    ) {
        for event in events {
            let (x, y) = event.position;
            match event.kind {
                PointerEventKind::Motion { .. } => self.pointer_motion(&event.surface, x, y),
                PointerEventKind::Leave { .. } => self.pointer_leave(&event.surface),
                PointerEventKind::Press { button, .. } => {
                    self.pointer_button(&event.surface, x, y, button);
                }
                // Enter needs nothing until something hovers on the bar itself;
                // releases and scrolling arrive with their first users (M9).
                PointerEventKind::Enter { .. }
                | PointerEventKind::Release { .. }
                | PointerEventKind::Axis { .. } => {}
            }
        }
    }
}
