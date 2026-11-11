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
//! **Hand cursor (a Rust-only addition, `docs/porting.md`):** over anything that
//! reacts to a click, the pointer shows a hand; elsewhere the arrow. It uses
//! the `wp_cursor_shape_v1` protocol: the bar only names the shape and the
//! compositor draws it from the user's own cursor theme, so no cursor images
//! are loaded. The shape is only sent when it changes, never per mouse move.
//!
//! Not ported yet, waiting for their first users: button releases and scrolling
//! (the volume/mixer sliders, M9).

use smithay_client_toolkit::{
    reexports::client::{
        Connection, QueueHandle,
        protocol::{wl_pointer::WlPointer, wl_seat::WlSeat, wl_surface::WlSurface},
    },
    reexports::protocols::wp::cursor_shape::v1::client::wp_cursor_shape_device_v1::{
        Shape, WpCursorShapeDeviceV1,
    },
    seat::{
        Capability, SeatHandler, SeatState,
        pointer::{AxisScroll, PointerEvent, PointerEventKind, PointerHandler},
    },
};

use super::{App, Output};
use crate::{
    audio::{Command, Kind, VOLUME_STEP},
    bar::popup_surface::PopupEnv,
    modules::powermenu::{PowerAction, PowerClick},
    render::{Action, BoxRect},
};

/// Linux input event codes for the mouse buttons (`linux/input-event-codes.h`).
pub(crate) const BTN_LEFT: u32 = 0x110;
/// Right button.
pub(crate) const BTN_RIGHT: u32 = 0x111;
/// Middle button (wheel click).
pub(crate) const BTN_MIDDLE: u32 = 0x112;

/// The mouse, plus what's needed to set its cursor shape.
pub(crate) struct Mouse {
    /// The pointer device itself.
    pointer: WlPointer,
    /// Sets this pointer's cursor shape; `None` if the compositor doesn't
    /// support `wp_cursor_shape_v1` (the cursor then just stays as it is).
    shape_device: Option<WpCursorShapeDeviceV1>,
    /// The serial of the latest `enter`. The protocol only accepts a shape
    /// change tagged with it, so a stale or made-up serial is ignored.
    enter_serial: u32,
    /// The shape last set since that enter; `None` right after an enter, when
    /// the cursor is undefined until the bar sets one.
    shape: Option<Shape>,
    /// Wheel movement not yet adding up to a whole notch, in 120ths
    /// (`App::scroll_steps`).
    scroll_120: i32,
}

impl Mouse {
    /// Lets go of the device (when the seat loses its mouse).
    fn release(self) {
        if let Some(device) = self.shape_device {
            device.destroy();
        }
        self.pointer.release();
    }
}

impl App {
    /// A button was pressed with the pointer at (x, y) on `surface`.
    pub(crate) fn pointer_button(&mut self, surface: &WlSurface, x: f64, y: f64, button: u32) {
        // Only the three main buttons do anything (side buttons are ignored, as
        // in C). Middle is bound only on the volume box (the mixer, M9c): the
        // power menu and the calendar treat any button as a click, so it's
        // kept away from them.
        if !matches!(button, BTN_LEFT | BTN_RIGHT | BTN_MIDDLE) {
            return;
        }
        // 0. The power menu covers whole monitors, so it goes first. Any button.
        match self.popups.power.click(surface, x, y) {
            PowerClick::NotOurs => {}
            PowerClick::Absorbed | PowerClick::Closed(None) => return,
            PowerClick::Closed(Some(action)) => {
                // NEVER in test mode: the tests click every card, and that must
                // not log out or reboot the machine. Don't remove this guard,
                // not even temporarily (docs/modules/power-menu.md).
                if !self.test_mode {
                    self.run_power_action(action);
                }
                return;
            }
        }

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

        // The CPU, GPU and memory dropdowns have nothing clickable inside: a
        // click on one is absorbed; its catcher closes it.
        if let Some(is_menu) = self.popups.sysmon_surface(surface) {
            if !is_menu {
                self.popups.close_sysmon();
            }
            return;
        }

        // The network dropdown, likewise: nothing clickable inside.
        if let Some(host) = self.popups.network.host() {
            if host.is_menu(surface) {
                return;
            }
            if host.is_catcher(surface) {
                self.popups.network.close();
                return;
            }
        }

        // 2. A bar: first the click targets inside boxes (workspace pills,
        // left button), then the module whose box is under the pointer.
        let Some(position) = self.outputs.iter().position(|o| {
            o.bar
                .as_ref()
                .is_some_and(|bar| bar.wl_surface() == surface)
        }) else {
            return;
        };
        let bar = self.outputs.get(position).and_then(|o| o.bar.as_ref());
        let target = bar
            .and_then(|bar| bar.targets().iter().find(|t| t.rect.contains(x, y)))
            .map(|t| t.action);
        if let Some(action) = target {
            if button == BTN_LEFT {
                self.run(action);
            }
            return;
        }
        // The module whose box was clicked, if any.
        let Some((name, rect)) = bar.and_then(|bar| {
            bar.boxes()
                .iter()
                .find(|b| b.rect.contains(x, y))
                .map(|b| (b.name, b.rect))
        }) else {
            return;
        };
        match (name, button) {
            // A left click on the Arch logo toggles the power menu.
            ("arch", BTN_LEFT) => self.toggle_power_menu(),
            // A right click on the CPU, GPU or memory box opens its dropdown
            // (reopens it if open, as in C). They open at the same spot, so
            // each replaces the others.
            ("cpu" | "gpu" | "mem", BTN_RIGHT) => self.open_sysmon_popup(name, position, rect),
            // A right click on the network box opens its dropdown (reopens it
            // if open). It's independent of the sysmon three.
            ("network", BTN_RIGHT) => self.open_network_popup(position, rect),
            // Right-click on the clock toggles the year calendar. Left does
            // nothing: the clock has no obvious primary action (docs/popups.md).
            ("clock", BTN_RIGHT) => self.toggle_calendar(position, rect),
            // A left click on volume or mic flips the default device's mute.
            ("volume", BTN_LEFT) => self.audio.send(Command::ToggleDefaultMute(Kind::Sink)),
            ("mic", BTN_LEFT) => self.audio.send(Command::ToggleDefaultMute(Kind::Source)),
            _ => {}
        }
    }

    /// One scroll step at (x, y) on `surface`: `up` is away from the user
    /// (raises a volume). Only the volume and mic boxes use it, ±5 % each.
    pub(crate) fn pointer_scroll(&mut self, surface: &WlSurface, x: f64, y: f64, up: bool) {
        let bar = self
            .outputs
            .iter()
            .find_map(|o| o.bar.as_ref().filter(|bar| bar.wl_surface() == surface));
        let kind = bar
            .and_then(|bar| bar.boxes().iter().find(|b| b.rect.contains(x, y)))
            .and_then(|b| match b.name {
                "volume" => Some(Kind::Sink),
                "mic" => Some(Kind::Source),
                _ => None,
            });
        if let Some(kind) = kind {
            let delta = if up { VOLUME_STEP } else { -VOLUME_STEP };
            self.audio.send(Command::AdjustDefault { kind, delta });
        }
    }

    /// Does what a click target does.
    fn run(&mut self, action: Action) {
        match action {
            Action::FocusWorkspace(id) => self.dispatcher.focus_workspace(id),
        }
    }

    /// The pointer moved to (x, y) on `surface`: hover highlights.
    pub(crate) fn pointer_motion(&mut self, surface: &WlSurface, x: f64, y: f64) {
        if self
            .popups
            .power
            .motion(surface, x, y, &mut self.renderer.text)
        {
            return;
        }
        if self
            .popups
            .calendar
            .host()
            .is_some_and(|h| h.is_menu(surface))
        {
            self.popups.calendar.motion(x, y, &mut self.renderer.text);
        }
    }

    /// The cursor shape for the pointer at (x, y) on `surface`: a hand over
    /// anything that reacts to a click (a dropdown's targets, a clickable bar
    /// box), the arrow everywhere else, catchers included.
    pub(crate) fn cursor_shape_at(&self, surface: &WlSurface, x: f64, y: f64) -> Shape {
        if self
            .popups
            .power
            .surfaces()
            .iter()
            .any(|s| s.wl_surface() == surface)
        {
            return if self.popups.power.is_card(surface, x, y) {
                Shape::Pointer
            } else {
                Shape::Default
            };
        }
        if self
            .popups
            .calendar
            .host()
            .is_some_and(|h| h.is_menu(surface))
        {
            return if self.popups.calendar.is_target(x, y) {
                Shape::Pointer
            } else {
                Shape::Default
            };
        }
        let over_clickable = self
            .outputs
            .iter()
            .filter_map(|o| o.bar.as_ref())
            .find(|bar| bar.wl_surface() == surface)
            .is_some_and(|bar| {
                bar.boxes()
                    .iter()
                    .any(|b| b.clickable && b.rect.contains(x, y))
                    || bar.targets().iter().any(|t| t.rect.contains(x, y))
            });
        if over_clickable {
            Shape::Pointer
        } else {
            Shape::Default
        }
    }

    /// Sets the real mouse's cursor to the right shape for (x, y) on
    /// `surface`, if it isn't already.
    fn update_cursor(&mut self, surface: &WlSurface, x: f64, y: f64) {
        let wanted = self.cursor_shape_at(surface, x, y);
        let Some(mouse) = self.pointer.as_mut() else {
            return;
        };
        if mouse.shape == Some(wanted) {
            return; // unchanged: nothing to send
        }
        if let Some(device) = &mouse.shape_device {
            device.set_shape(mouse.enter_serial, wanted);
            mouse.shape = Some(wanted);
        }
    }

    /// The pointer left `surface`: drop any hover highlight there.
    fn pointer_leave(&mut self, surface: &WlSurface) {
        // A point outside the popup matches no target, clearing the hover.
        self.pointer_motion(surface, -1.0, -1.0);
    }

    /// Opens the power menu on every monitor, or closes it (the Arch logo's
    /// left click and the SIGUSR1 keybind).
    pub(crate) fn toggle_power_menu(&mut self) {
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
        let env = PopupEnv {
            qh,
            compositor,
            layer_shell,
            viewporter: viewporter.as_ref(),
            test_mode: *test_mode,
        };
        popups.power.toggle(
            &env,
            outputs.iter().map(Output::wl_output),
            &mut renderer.text,
        );
    }

    /// Opens the `name` ("cpu", "gpu" or "mem") dropdown under the box
    /// `trigger` on the monitor at `position` in `outputs`, closing the other
    /// two first.
    fn open_sysmon_popup(&mut self, name: &str, position: usize, trigger: BoxRect) {
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
        let all = outputs.iter().map(Output::wl_output);
        let (sysmon, text) = (&mut renderer.sysmon, &mut renderer.text);
        popups.close_sysmon();
        match name {
            "cpu" => popups
                .cpu
                .open(&env, output.wl_output(), trigger, all, sysmon, text),
            "gpu" => popups
                .gpu
                .open(&env, output.wl_output(), trigger, all, sysmon, text),
            _ => popups
                .mem
                .open(&env, output.wl_output(), trigger, all, sysmon, text),
        }
    }

    /// Opens the network dropdown under the box `trigger` on the monitor at
    /// `position` in `outputs`.
    fn open_network_popup(&mut self, position: usize, trigger: BoxRect) {
        let Some(output_width) = self.outputs.get(position).map(|o| self.output_size(o).0) else {
            return;
        };
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
        popups.network.open(
            &env,
            (output.wl_output(), output_width),
            trigger,
            outputs.iter().map(Output::wl_output),
            &mut renderer.network,
            &mut renderer.text,
        );
    }

    /// Runs a power menu card's action.
    fn run_power_action(&mut self, action: PowerAction) {
        match action {
            PowerAction::Logout => self.dispatcher.exit(),
            PowerAction::Restart => spawn_systemctl("reboot"),
            PowerAction::Shutdown => spawn_systemctl("poweroff"),
        }
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

/// Starts `systemctl <verb>` without blocking the event loop. A named thread
/// waits for it, so a failure (e.g. polkit refusing) is logged and the process
/// is reaped instead of lingering as a zombie. The thread starts long after the
/// signal source, so it inherits the blocked SIGUSR1 like every other thread.
fn spawn_systemctl(verb: &'static str) {
    let mut child = match std::process::Command::new("systemctl").arg(verb).spawn() {
        Ok(child) => child,
        Err(err) => {
            eprintln!("stickBar: failed to launch 'systemctl {verb}': {err}");
            return;
        }
    };
    let waiter = std::thread::Builder::new()
        .name("power-action".to_owned())
        .spawn(move || match child.wait() {
            Ok(status) if !status.success() => {
                eprintln!("stickBar: 'systemctl {verb}' failed ({status})");
            }
            Ok(_) => {}
            Err(err) => eprintln!("stickBar: waiting for 'systemctl {verb}' failed: {err}"),
        });
    if let Err(err) = waiter {
        eprintln!("stickBar: couldn't start a thread to wait for 'systemctl {verb}': {err}");
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
                Ok(pointer) => {
                    let shape_device = self
                        .cursor_shapes
                        .as_ref()
                        .map(|m| m.get_shape_device(&pointer, qh));
                    self.pointer = Some(Mouse {
                        pointer,
                        shape_device,
                        enter_serial: 0,
                        shape: None,
                        scroll_120: 0,
                    });
                }
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
            && let Some(mouse) = self.pointer.take()
        {
            mouse.release();
        }
    }

    fn remove_seat(&mut self, _: &Connection, _: &QueueHandle<Self>, _: WlSeat) {}
}

impl App {
    /// Turns one vertical scroll event into whole wheel steps (`true` = up).
    ///
    /// A seat at version 8+ (SCTK binds up to 10) reports wheels in 120ths of
    /// a notch (`value120`): one notch is 120, a high-resolution wheel sends
    /// smaller parts. Those are added up and one step is taken per full 120,
    /// so a notch is one ±5 % step either way. Older seats send `discrete`
    /// notches instead (the C bar's case; it binds version 7). Touchpad-only
    /// scrolling has neither and is ignored, as in C. Rust-only difference:
    /// C counts each event as one step whatever its size.
    fn scroll_steps(&mut self, vertical: AxisScroll) -> Vec<bool> {
        let Some(mouse) = self.pointer.as_mut() else {
            return Vec::new();
        };
        let notches = if vertical.value120 != 0 {
            mouse.scroll_120 += vertical.value120;
            let whole = mouse.scroll_120 / 120;
            mouse.scroll_120 %= 120;
            whole
        } else {
            vertical.discrete
        };
        // Positive is towards the user (like scrolling a page down): volume down.
        let up = notches < 0;
        (0..notches.unsigned_abs()).map(|_| up).collect()
    }
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
                PointerEventKind::Enter { serial } => {
                    // A new enter: the cursor is undefined until set, so the
                    // shape is always sent, tagged with this enter's serial.
                    if let Some(mouse) = self.pointer.as_mut() {
                        mouse.enter_serial = serial;
                        mouse.shape = None;
                    }
                    self.update_cursor(&event.surface, x, y);
                }
                PointerEventKind::Motion { .. } => {
                    self.pointer_motion(&event.surface, x, y);
                    self.update_cursor(&event.surface, x, y);
                }
                PointerEventKind::Leave { .. } => {
                    self.pointer_leave(&event.surface);
                    if let Some(mouse) = self.pointer.as_mut() {
                        mouse.shape = None;
                    }
                }
                PointerEventKind::Press { button, .. } => {
                    self.pointer_button(&event.surface, x, y, button);
                    // A click can open or close a dropdown under the pointer.
                    self.update_cursor(&event.surface, x, y);
                }
                PointerEventKind::Axis { vertical, .. } => {
                    for up in self.scroll_steps(vertical) {
                        self.pointer_scroll(&event.surface, x, y, up);
                    }
                }
                // Releases arrive with their first users (the dropdown drags, M9b).
                PointerEventKind::Release { .. } => {}
            }
        }
    }
}
