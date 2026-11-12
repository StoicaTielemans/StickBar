//! The D-Bus session bus, for the system tray (the C bar's `dbus/`; MPRIS
//! joins in M12). `docs/modules/tray.md` has the protocol details.
//!
//! **One `dbus` thread owns the connection.** The C bar drove sd-bus from its
//! main loop with blocking calls, so a tray app that didn't answer could
//! freeze the whole bar for sd-bus's 25 s timeout (`docs/limitations.md`).
//! Here every D-Bus call happens on this thread: the main loop only gets
//! [`TrayEvent`]s on a calloop channel and sends [`Command`]s back, and never
//! waits for an app.
//!
//! Inside, the thread runs one async loop (zbus's own `block_on`) over the
//! merged message streams it cares about plus the command channel, handling
//! one thing at a time. Calls time out after [`METHOD_TIMEOUT`].
//! **Back-pressure rule:** a zbus stream whose queue is full makes zbus's
//! socket reader wait for room, which also holds up the replies this thread
//! is waiting for. So the queues are generous ([`QUEUE_LEN`]), and the worst
//! case is a call timing out, never a stuck bar.

mod dbusmenu;
mod icon_theme;
mod sni;
mod sni_icon;
mod watcher;

use std::time::Duration;

use calloop::{LoopHandle, channel};
use futures_lite::StreamExt;
use zbus::{Connection, MatchRule, MessageStream, message::Type};

pub(crate) use dbusmenu::{MenuEntry, Toggle, ToggleState};
pub(crate) use sni_icon::Argb;

use crate::wayland::App;

/// How long a call may take before it's given up on. Long enough for any app
/// that's alive (they answer in milliseconds); short enough that one hung app
/// only briefly delays the others.
const METHOD_TIMEOUT: Duration = Duration::from_secs(3);
/// Messages each stream may hold before zbus's reader waits (see module docs).
const QUEUE_LEN: usize = 256;

/// A tray icon's picture, as the `dbus` thread found it.
#[derive(Debug)]
pub(crate) enum Icon {
    /// From `IconPixmap`, already premultiplied in cairo's byte order.
    Argb(Argb),
    /// A PNG file's bytes, from the icon-theme fallback.
    Png(Vec<u8>),
}

/// What the main thread learns about the tray.
#[derive(Debug)]
pub(crate) enum TrayEvent {
    /// A new item (appended: the tray keeps registration order).
    Added {
        /// The bar's own id for it (never reused).
        id: u64,
        /// SNI `Title`, or its `Id` without one ("" if neither).
        title: String,
        /// It has a menu: any click opens it (the reference bar's rule).
        has_menu: bool,
        /// Its picture, if one was found.
        icon: Option<Icon>,
    },
    /// An item's icon changed (`NewIcon`).
    Icon {
        /// Which item.
        id: u64,
        /// The new picture.
        icon: Icon,
    },
    /// An item went away.
    Removed {
        /// Which item.
        id: u64,
    },
    /// The answer to [`Command::FetchMenu`] (empty if it failed).
    Menu {
        /// The request it answers.
        request: u64,
        /// The top-level entries.
        entries: Vec<MenuEntry>,
    },
}

/// What the main thread asks for.
#[derive(Debug)]
pub(crate) enum Command {
    /// Fetch item `item`'s menu; answered with [`TrayEvent::Menu`].
    FetchMenu {
        /// Echoed back, so a stale answer can be told apart.
        request: u64,
        /// Which item.
        item: u64,
    },
    /// A menu entry was clicked.
    MenuClick {
        /// Which item.
        item: u64,
        /// The entry's id.
        entry: i32,
    },
    /// A menu-less item was left-clicked: `Activate(0, 0)`.
    Activate {
        /// Which item.
        item: u64,
    },
}

/// The main thread's end: sends commands to the `dbus` thread.
#[derive(Debug, Default)]
pub(crate) struct Dbus {
    /// `None` when the thread couldn't start (the tray then stays empty).
    commands: Option<async_channel::Sender<Command>>,
}

impl Dbus {
    /// Queues `command` (never blocks; a no-op without a `dbus` thread).
    pub(crate) fn send(&self, command: Command) {
        if let Some(commands) = &self.commands
            && commands.try_send(command).is_err()
        {
            eprintln!("stickBar: tray: the D-Bus thread is gone");
        }
    }
}

/// Starts the `dbus` thread. Events arrive through the main loop at
/// `App::tray_event`. If the thread can't start, says so once: the tray
/// stays empty and the bar runs on (C's "degrade gracefully").
pub(crate) fn start(handle: &LoopHandle<'static, App>) -> Dbus {
    let (events, event_source) = channel::channel::<TrayEvent>();
    let inserted = handle.insert_source(event_source, |event, (), app| {
        if let channel::Event::Msg(event) = event {
            app.tray_event(event);
        }
    });
    if let Err(err) = inserted {
        eprintln!(
            "stickBar: tray: couldn't watch the D-Bus channel ({}) -- tray disabled",
            err.error
        );
        return Dbus::default();
    }
    let (commands, command_queue) = async_channel::unbounded();
    // Named, so it shows in `top -H` and the tests' thread checks. Started
    // after the signal setup in `main`, so it inherits the SIGUSR1 block (and
    // passes it on to the `async-io` reactor thread zbus's runtime starts).
    let started = std::thread::Builder::new()
        .name("dbus".to_owned())
        .spawn(move || zbus::block_on(run(events, command_queue)));
    match started {
        // Detached: it lives as long as the bar.
        Ok(_detached) => Dbus {
            commands: Some(commands),
        },
        Err(err) => {
            eprintln!("stickBar: tray: couldn't start the D-Bus thread ({err}) -- tray disabled");
            Dbus::default()
        }
    }
}

/// The value inside any number of variant wrappers (property values and
/// menu children arrive wrapped).
fn unwrap_variant<'a>(value: &'a zbus::zvariant::Value<'a>) -> &'a zbus::zvariant::Value<'a> {
    if let zbus::zvariant::Value::Value(inner) = value {
        unwrap_variant(inner)
    } else {
        value
    }
}

/// One thing for the loop to handle.
enum Input {
    /// A call to our Watcher object (Watcher mode).
    WatcherCall(zbus::Result<zbus::Message>),
    /// A signal from the Watcher we registered with (Host mode).
    WatcherSignal(zbus::Result<zbus::Message>),
    /// `NameOwnerChanged`: an app may have quit.
    OwnerChanged(zbus::Result<zbus::Message>),
    /// An item's `NewIcon`.
    NewIcon(zbus::Result<zbus::Message>),
    /// A command from the main thread.
    Command(Command),
}

/// The thread's body: connect, become the Watcher or a Host, then handle
/// whatever arrives until the bar exits.
async fn run(events: channel::Sender<TrayEvent>, commands: async_channel::Receiver<Command>) {
    let conn = match connect().await {
        Ok(conn) => conn,
        Err(err) => {
            eprintln!("stickBar: tray: no D-Bus session bus ({err}) -- tray stays empty");
            return;
        }
    };
    // zbus's own tasks (reading the socket, routing messages to streams) run
    // on this thread too, interleaved with the tray's loop.
    match futures_lite::future::or(serve(&conn, events, commands), drive(&conn)).await {
        Ok(()) => {}
        Err(err) => eprintln!("stickBar: tray: D-Bus setup failed ({err}) -- tray stays empty"),
    }
}

/// Runs zbus's executor forever. With `internal_executor(false)` zbus starts
/// no thread of its own for it (saving one), so this thread must tick it.
async fn drive(conn: &Connection) -> zbus::Result<()> {
    loop {
        conn.executor().tick().await;
    }
}

/// The session bus, with the short call timeout, and zbus's executor run by
/// this thread (see [`drive`]) instead of a thread of zbus's own.
async fn connect() -> zbus::Result<Connection> {
    zbus::connection::Builder::session()?
        .method_timeout(METHOD_TIMEOUT)
        .internal_executor(false)
        .build()
        .await
}

/// A stream of the messages matching `rule`.
async fn stream(conn: &Connection, rule: MatchRule<'static>) -> zbus::Result<MessageStream> {
    MessageStream::for_match_rule(rule, conn, Some(QUEUE_LEN)).await
}

/// Subscribes, picks the mode, loads the items there already, then loops.
async fn serve(
    conn: &Connection,
    events: channel::Sender<TrayEvent>,
    commands: async_channel::Receiver<Command>,
) -> zbus::Result<()> {
    // Subscribed before anything is registered, so nothing is missed.
    let owner_changes = stream(
        conn,
        MatchRule::builder()
            .msg_type(Type::Signal)
            .sender("org.freedesktop.DBus")?
            .interface("org.freedesktop.DBus")?
            .member("NameOwnerChanged")?
            .build(),
    )
    .await?;
    let new_icons = stream(
        conn,
        MatchRule::builder()
            .msg_type(Type::Signal)
            .interface(sni::ITEM_INTERFACE)?
            .member("NewIcon")?
            .build(),
    )
    .await?;

    let mut tray = sni::Registry::new(conn.clone(), events);
    let (is_watcher, watcher_input) = match watcher::start(conn).await? {
        watcher::Mode::Watcher(calls) => (true, calls.map(Input::WatcherCall).boxed()),
        watcher::Mode::Host { signals, items } => {
            for entry in &items {
                let (bus_name, path) = watcher::split_bus_name_path(entry);
                tray.add(&bus_name, &path).await;
            }
            (false, signals.map(Input::WatcherSignal).boxed())
        }
        // No Watcher at all: no items can reach the bar, so there's nothing
        // to follow (the commands still work, for nothing).
        watcher::Mode::None => (false, futures_lite::stream::pending::<Input>().boxed()),
    };

    // Pinned: the command channel's stream can't move once polled.
    let mut inputs = std::pin::pin!(
        watcher_input
            .race(owner_changes.map(Input::OwnerChanged))
            .race(new_icons.map(Input::NewIcon))
            .race(commands.map(Input::Command))
    );
    while let Some(input) = inputs.next().await {
        match input {
            Input::WatcherCall(Ok(msg)) => watcher::handle_call(conn, &msg, &mut tray).await,
            Input::WatcherSignal(Ok(msg)) => watcher::handle_signal(&msg, &mut tray).await,
            Input::OwnerChanged(Ok(msg)) => {
                // (name, old owner, new owner): an empty new owner means the
                // name's owner disconnected.
                if let Ok((name, _old, new)) = msg.body().deserialize::<(String, String, String)>()
                    && new.is_empty()
                {
                    let removed = tray.remove_owner(&name);
                    if is_watcher {
                        for entry in removed {
                            watcher::emit_unregistered(conn, &entry).await;
                        }
                    }
                }
            }
            Input::NewIcon(Ok(msg)) => tray.new_icon(&msg).await,
            Input::Command(command) => tray.command(command).await,
            Input::WatcherCall(Err(err))
            | Input::WatcherSignal(Err(err))
            | Input::OwnerChanged(Err(err))
            | Input::NewIcon(Err(err)) => {
                eprintln!("stickBar: tray: a bad D-Bus message ({err})");
            }
        }
    }
    Ok(())
}
