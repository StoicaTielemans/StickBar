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
        protocol::{
            wl_output::WlOutput, wl_pointer::WlPointer, wl_seat::WlSeat, wl_surface::WlSurface,
        },
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
    bar::{popup_surface::PopupEnv, strip_surface::StripEnv},
    config::{COLORS, Color},
    dbus::{self, MenuEntry, TrayCommand},
    modules::{
        media::popup::MediaPress,
        powermenu::{PowerAction, PowerClick},
    },
    mpris::MediaCommand,
    render::{Action, BoxRect},
};

/// Linux input event codes for the mouse buttons (`linux/input-event-codes.h`).
pub(crate) const BTN_LEFT: u32 = 0x110;
/// Right button.
pub(crate) const BTN_RIGHT: u32 = 0x111;
/// Middle button (wheel click).
pub(crate) const BTN_MIDDLE: u32 = 0x112;

/// A tray menu asked of the `dbus` thread: it opens when the answer comes
/// back, under the icon that was clicked. Waiting there instead of here keeps
/// a slow app from ever stalling the bar (C fetched it in the click handler).
#[derive(Debug)]
pub(crate) struct PendingMenu {
    /// Its request number: an answer to an older click is dropped.
    request: u64,
    /// Which tray item.
    item: u64,
    /// The monitor whose bar was clicked.
    output: WlOutput,
    /// The icon's rect on that bar.
    trigger: BoxRect,
}

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
        // in C).
        if !matches!(button, BTN_LEFT | BTN_RIGHT | BTN_MIDDLE) {
            return;
        }

        // The mixer's surfaces first, as in C: it's the one popup opened by
        // the middle button. Its menu takes left presses; its catcher
        // closes it (any button).
        if let Some(host) = self.popups.mixer.host() {
            if host.is_menu(surface) {
                if button == BTN_LEFT {
                    let command = self.popups.mixer.press(
                        (x, y),
                        &self.renderer.audio,
                        &mut self.renderer.text,
                    );
                    self.send_audio(command);
                }
                return;
            }
            if host.is_catcher(surface) {
                self.popups.mixer.close();
                return;
            }
        }
        // The middle button is bound only on the volume box (the mixer, which
        // replaces the device dropdown). Kept away from everything below:
        // the power menu and the calendar treat any button as a click.
        if button == BTN_MIDDLE {
            if let Some((position, rect)) = self.bar_box(surface, x, y, "volume") {
                self.popups.device.close();
                self.open_mixer_popup(position, rect);
            }
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

        // 1. The other dropdowns.
        if self.dropdown_button(surface, x, y, button) {
            return;
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
            .map(|t| (t.action, t.rect));
        if let Some((action, rect)) = target {
            self.run(action, button, (position, rect));
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
            // A right click on the Arch logo toggles the power menu (right,
            // like every other box's menu, at the user's request; left does
            // nothing).
            ("arch", BTN_RIGHT) => self.toggle_power_menu(),
            // A right click on the CPU, GPU or memory box opens its dropdown
            // (reopens it if open, as in C). They open at the same spot, so
            // each replaces the others.
            ("cpu" | "gpu" | "mem", BTN_RIGHT) => self.open_sysmon_popup(name, position, rect),
            // A right click on the network box opens its dropdown (reopens it
            // if open). It's independent of the sysmon three.
            ("network", BTN_RIGHT) => self.open_network_popup(position, rect),
            // The media box: left plays/pauses the player it shows (nothing
            // when it's the idle box, with no player), right opens the
            // dropdown of every player plus the EQ card (reopens it if open).
            ("media", BTN_LEFT) => {
                if let Some(player) = self.renderer.media.active().map(|p| p.id) {
                    self.dbus
                        .send(dbus::Command::Media(MediaCommand::PlayPause { player }));
                }
            }
            ("media", BTN_RIGHT) => self.open_media_popup(position, rect),
            // A right click on the privacy box lists who's recording.
            ("privacy", BTN_RIGHT) => self.open_privacy_popup(position, rect),
            // Right-click on the clock toggles the year calendar. Left does
            // nothing: the clock has no obvious primary action (docs/popups.md).
            ("clock", BTN_RIGHT) => self.toggle_calendar(position, rect),
            // A left click on volume or mic flips the default device's mute.
            ("volume", BTN_LEFT) => self.audio.send(Command::ToggleDefaultMute(Kind::Sink)),
            ("mic", BTN_LEFT) => self.audio.send(Command::ToggleDefaultMute(Kind::Source)),
            // A right click opens the device dropdown for that side.
            ("volume", BTN_RIGHT) => {
                self.popups.mixer.close();
                self.open_device_popup(position, rect, (Kind::Sink, COLORS.sky));
            }
            ("mic", BTN_RIGHT) => {
                self.open_device_popup(position, rect, (Kind::Source, COLORS.blue));
            }
            _ => {}
        }
    }

    /// A press on one of the dropdowns with nothing clickable inside (CPU,
    /// GPU, memory, network, privacy): absorbed on the menu, closing on a
    /// catcher. Returns whether `surface` belonged to one.
    fn read_only_dropdown_button(&mut self, surface: &WlSurface) -> bool {
        // The CPU, GPU and memory dropdowns have nothing clickable inside: a
        // click on one is absorbed; its catcher closes it.
        if let Some(is_menu) = self.popups.sysmon_surface(surface) {
            if !is_menu {
                self.popups.close_sysmon();
            }
            return true;
        }

        // The privacy dropdown: nothing clickable inside; its catcher closes it.
        if let Some(host) = self.popups.privacy.host() {
            if host.is_menu(surface) {
                return true;
            }
            if host.is_catcher(surface) {
                self.popups.privacy.close();
                return true;
            }
        }

        // The network dropdown.
        if let Some(host) = self.popups.network.host() {
            if host.is_menu(surface) {
                return true;
            }
            if host.is_catcher(surface) {
                self.popups.network.close();
                return true;
            }
        }
        false
    }

    /// A press at (x, y) on the media dropdown: a player chip or a seekable
    /// timeline sends its command (any button, as in C); an EQ chip applies
    /// its profile, unless a switch is already under way. The dropdown stays
    /// open either way.
    fn media_press(&mut self, x: f64, y: f64) {
        let command = match self.popups.media.press((x, y), &self.renderer.media) {
            None => return,
            Some(MediaPress::EqProfile(name)) => {
                self.renderer.media.eq.apply(name);
                // The chip and pill turn to "applying" now, and the bar's
                // sparkline yellow; the new curve comes with the state write.
                self.eq_state_changed();
                return;
            }
            Some(MediaPress::Player(command)) => command,
        };
        if let MediaCommand::Seek {
            player,
            position_us,
        } = command
        {
            // Shown at once; the next poll corrects it if the player clamped
            // or ignored the seek.
            self.renderer.media.set_position(player, position_us);
            let renderer = &mut self.renderer;
            self.popups.media.paint(&renderer.media, &mut renderer.text);
        }
        self.dbus.send(dbus::Command::Media(command));
    }

    /// A press on one of the dropdowns below the power menu: the calendar,
    /// the sysmon three, the device dropdown or the network one. Returns
    /// whether `surface` belonged to one (the press is then used up).
    fn dropdown_button(&mut self, surface: &WlSurface, x: f64, y: f64, button: u32) -> bool {
        if self.read_only_dropdown_button(surface) {
            return true;
        }

        // The calendar's own menu absorbs the click; its catcher closes it.
        if let Some(host) = self.popups.calendar.host() {
            if host.is_menu(surface) {
                self.popups
                    .calendar
                    .click(x, y, button, &mut self.renderer.text);
                return true;
            }
            if host.is_catcher(surface) {
                self.popups.calendar.close();
                return true;
            }
        }

        // The device dropdown: its cards take left presses (meter, mute,
        // set default); its catcher closes it.
        if let Some(host) = self.popups.device.host() {
            if host.is_menu(surface) {
                if button == BTN_LEFT {
                    let command = self.popups.device.press(
                        (x, y),
                        &self.renderer.audio,
                        &mut self.renderer.text,
                    );
                    self.send_audio(command);
                }
                return true;
            }
            if host.is_catcher(surface) {
                self.popups.device.close();
                return true;
            }
        }

        // The media dropdown: any button on a chip or a seekable timeline
        // sends its command (as in C), and it stays open; its catcher closes it.
        if let Some(host) = self.popups.media.host() {
            if host.is_menu(surface) {
                self.media_press(x, y);
                return true;
            }
            if host.is_catcher(surface) {
                self.popups.media.close();
                return true;
            }
        }

        // A tray menu: any button on an enabled entry sends its click event
        // and closes the menu (as in C); its catcher closes it.
        if let Some(host) = self.popups.tray.host() {
            if host.is_menu(surface) {
                if let Some((item, entry)) = self.popups.tray.press((x, y)) {
                    self.dbus
                        .send(dbus::Command::Tray(TrayCommand::MenuClick { item, entry }));
                }
                return true;
            }
            if host.is_catcher(surface) {
                self.popups.tray.close();
                return true;
            }
        }

        false
    }

    /// Sends an audio command, if there is one.
    fn send_audio(&mut self, command: Option<Command>) {
        if let Some(command) = command {
            self.audio.send(command);
        }
    }

    /// A button was released, wherever: ends a volume drag. Wayland keeps
    /// sending a dragging pointer's events to the surface it pressed on, but
    /// the release is handled unconditionally anyway, so a drag always ends.
    pub(crate) fn pointer_release(&mut self) {
        let (audio, text) = (&self.renderer.audio, &mut self.renderer.text);
        self.popups.mixer.release(audio, text);
        self.popups.device.release(audio, text);
    }

    /// The bar box called `name` under (x, y), if `surface` is a bar and the
    /// point is on it: its monitor's position in `outputs`, and the box.
    fn bar_box(&self, surface: &WlSurface, x: f64, y: f64, name: &str) -> Option<(usize, BoxRect)> {
        let position = self.outputs.iter().position(|o| {
            o.bar
                .as_ref()
                .is_some_and(|bar| bar.wl_surface() == surface)
        })?;
        let bar = self.outputs.get(position)?.bar.as_ref()?;
        let found = bar
            .boxes()
            .iter()
            .find(|b| b.name == name && b.rect.contains(x, y))?;
        Some((position, found.rect))
    }

    /// One scroll step at (x, y) on `surface`: `up` is away from the user
    /// (raises a volume). ±5 % on a device card, or on the volume or mic box.
    pub(crate) fn pointer_scroll(&mut self, surface: &WlSurface, x: f64, y: f64, up: bool) {
        if self.popups.mixer.host().is_some_and(|h| h.is_menu(surface)) {
            let command = self.popups.mixer.scroll((x, y), up);
            self.send_audio(command);
            return;
        }
        if self
            .popups
            .device
            .host()
            .is_some_and(|h| h.is_menu(surface))
        {
            let command = self
                .popups
                .device
                .scroll((x, y), up, &mut self.renderer.text);
            self.send_audio(command);
            return;
        }
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
    fn run(&mut self, action: Action, button: u32, (position, rect): (usize, BoxRect)) {
        match action {
            // Pills only take the left button.
            Action::FocusWorkspace(id) => {
                if button == BTN_LEFT {
                    self.dispatcher.focus_workspace(id);
                }
            }
            Action::TrayIcon(id) => self.tray_click(id, button, (position, rect)),
        }
    }

    /// A click on tray icon `id` (at `rect` on the bar at `position`). If the
    /// item has a menu, **any** button asks for it; `Activate` only happens
    /// for a menu-less item, and only on the left button. That's the
    /// reference bar's rule, easy to get backwards (docs/modules/tray.md).
    fn tray_click(&mut self, id: u64, button: u32, (position, rect): (usize, BoxRect)) {
        let Some(item) = self.renderer.tray.item(id) else {
            return;
        };
        if item.has_menu {
            let Some(output) = self.outputs.get(position).map(|o| o.wl_output().clone()) else {
                return;
            };
            // At most one tray menu: the old one goes now, the new one opens
            // when its entries arrive.
            self.popups.tray.close();
            self.tray_requests += 1;
            let request = self.tray_requests;
            self.tray_pending = Some(PendingMenu {
                request,
                item: id,
                output,
                trigger: rect,
            });
            self.dbus.send(dbus::Command::Tray(TrayCommand::FetchMenu {
                request,
                item: id,
            }));
        } else if button == BTN_LEFT {
            self.dbus
                .send(dbus::Command::Tray(TrayCommand::Activate { item: id }));
        }
    }

    /// The `dbus` thread answered menu request `request`: opens it under its
    /// icon, unless a newer click has replaced it or it came back empty.
    pub(crate) fn open_tray_menu(&mut self, request: u64, entries: Vec<MenuEntry>) {
        let Some(pending) = self.tray_pending.take_if(|p| p.request == request) else {
            return;
        };
        let Some(position) = self
            .outputs
            .iter()
            .position(|o| o.wl_output() == &pending.output)
        else {
            return; // that monitor is gone
        };
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
        let Some(title) = renderer.tray.item(pending.item).map(|i| i.title.clone()) else {
            return; // the item went away meanwhile
        };
        let env = PopupEnv {
            qh,
            compositor,
            layer_shell,
            viewporter: viewporter.as_ref(),
            test_mode: *test_mode,
        };
        popups.tray.open(
            &env,
            (&pending.output, output_width, pending.trigger),
            outputs.iter().map(Output::wl_output),
            (pending.item, &title, entries),
            &renderer.tray,
            &mut renderer.text,
        );
    }

    /// The pointer moved to (x, y) on `surface`: hover highlights.
    pub(crate) fn pointer_motion(&mut self, surface: &WlSurface, x: f64, y: f64) {
        // The mixer and the device dropdown: a drag follows the pointer (which
        // keeps reporting to the menu surface while the button is held),
        // otherwise hover highlights.
        if self.popups.mixer.host().is_some_and(|h| h.is_menu(surface)) {
            let command =
                self.popups
                    .mixer
                    .motion((x, y), &self.renderer.audio, &mut self.renderer.text);
            self.send_audio(command);
            return;
        }
        if self
            .popups
            .device
            .host()
            .is_some_and(|h| h.is_menu(surface))
        {
            let command =
                self.popups
                    .device
                    .motion((x, y), &self.renderer.audio, &mut self.renderer.text);
            self.send_audio(command);
            return;
        }
        if self
            .popups
            .power
            .motion(surface, x, y, &mut self.renderer.text)
        {
            return;
        }
        if self.popups.tray.host().is_some_and(|h| h.is_menu(surface)) {
            self.popups
                .tray
                .motion((x, y), &self.renderer.tray, &mut self.renderer.text);
            return;
        }
        if self.popups.media.host().is_some_and(|h| h.is_menu(surface)) {
            self.popups
                .media
                .motion((x, y), &self.renderer.media, &mut self.renderer.text);
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
        // The mixer's chips and sliders react to a click.
        if self.popups.mixer.host().is_some_and(|h| h.is_menu(surface)) {
            return if self.popups.mixer.over_control(x, y) {
                Shape::Pointer
            } else {
                Shape::Default
            };
        }
        // An enabled media chip, or a seekable timeline, reacts to a click.
        if self.popups.media.host().is_some_and(|h| h.is_menu(surface)) {
            return if self
                .popups
                .media
                .over_clickable((x, y), &self.renderer.media)
            {
                Shape::Pointer
            } else {
                Shape::Default
            };
        }
        // An enabled tray menu entry reacts to a click.
        if self.popups.tray.host().is_some_and(|h| h.is_menu(surface)) {
            return if self.popups.tray.over_clickable((x, y)) {
                Shape::Pointer
            } else {
                Shape::Default
            };
        }
        // Every part of a device card reacts to a click.
        if self
            .popups
            .device
            .host()
            .is_some_and(|h| h.is_menu(surface))
        {
            return if self.popups.device.over_card(x, y) {
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

    /// Opens the mixer under the volume box `trigger` on the monitor at
    /// `position` in `outputs`.
    fn open_mixer_popup(&mut self, position: usize, trigger: BoxRect) {
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
        popups.mixer.open(
            &env,
            (output.wl_output(), output_width),
            (trigger, COLORS.sky),
            outputs.iter().map(Output::wl_output),
            (&renderer.audio, &mut renderer.text),
        );
    }

    /// Opens the device dropdown for `kind` under the box `trigger` on the
    /// monitor at `position` in `outputs`, in the box's colour.
    fn open_device_popup(
        &mut self,
        position: usize,
        trigger: BoxRect,
        (kind, accent): (Kind, Color),
    ) {
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
        popups.device.open(
            &env,
            (output.wl_output(), output_width),
            (trigger, kind, accent),
            outputs.iter().map(Output::wl_output),
            (&renderer.audio, &mut renderer.text),
        );
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

    /// Opens the privacy dropdown under the box `trigger` on the monitor at
    /// `position` in `outputs`, with the streams as they are now.
    fn open_privacy_popup(&mut self, position: usize, trigger: BoxRect) {
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
        popups.privacy.open(
            &env,
            (output.wl_output(), trigger),
            outputs.iter().map(Output::wl_output),
            &renderer.privacy,
            &mut renderer.text,
        );
    }

    /// Opens the media dropdown under the box `trigger` on the monitor at
    /// `position` in `outputs`, and asks for the players' positions (the
    /// answer repaints it: the first frame shows the last known ones).
    fn open_media_popup(&mut self, position: usize, trigger: BoxRect) {
        let Self {
            qh,
            compositor,
            layer_shell,
            viewporter,
            subcompositor,
            shm,
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
        let strips = subcompositor.as_ref().map(|subcompositor| StripEnv {
            qh,
            compositor,
            subcompositor,
            shm,
        });
        popups.media.open(
            (&env, strips.as_ref()),
            (output.wl_output(), trigger),
            outputs.iter().map(Output::wl_output),
            &renderer.media,
            &mut renderer.text,
        );
        if self.popups.media.is_open() {
            self.dbus
                .send(dbus::Command::Media(MediaCommand::PollPositions));
        }
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
                PointerEventKind::Release { .. } => self.pointer_release(),
            }
        }
    }
}
