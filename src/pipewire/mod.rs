//! `PipeWire`, for the privacy indicator and the media visualizer's capture
//! (the C bar's `pipewire/` and the capture half of `visualizer/`;
//! `docs/modules/privacy.md` and `docs/modules/media.md` have the details).
//! libpulse, which the volume and mic use, can't see the stream graph or
//! video at all; `PipeWire` can.
//!
//! **A `pipewire` thread owns the connection** (pipewire-rs's main loop, the
//! C bar's `pw_thread_loop`). It follows the registry: every node's
//! `media.class`, name, serial and client, every link's two ends, every
//! client's process id, and the running state of recording and playback
//! streams (bound, with an info listener). After each change it works out
//! who's recording what ([`graph::Graph::privacy_state`]) and sends that to
//! the main loop **only if it differs from the last one sent**, so the
//! graph's constant churn costs the main thread nothing.
//!
//! The visualizer's slots run here too ([`capture`]): commands arrive on a
//! `pipewire::channel` attached to this loop, and each active slot's stream
//! follows its player's own playback stream, re-resolved whenever playback
//! streams or client pids change.

pub(crate) mod capture;
mod graph;

use std::{cell::RefCell, collections::HashMap, rc::Rc};

use calloop::{LoopHandle, channel};
use pipewire::{
    self as pw,
    client::{Client, ClientListener},
    core::CoreRc,
    node::{Node, NodeListener, NodeState},
    registry::GlobalObject,
    spa::utils::dict::DictRef,
    types::ObjectType,
};

use capture::{CaptureCommand, SharedRings};
pub(crate) use graph::{Category, PrivacyState};

use crate::{visualizer::MAX_SOURCES, wayland::App};

/// What the main loop hears from the `pipewire` thread.
#[derive(Debug)]
pub(crate) enum PipewireEvent {
    /// Who's recording what changed.
    Privacy(PrivacyState),
    /// Visualizer slot `slot` now captures `target` (a playback stream's
    /// serial, or `capture::TARGET_MIX`).
    Target {
        /// Which slot.
        slot: usize,
        /// What it captures.
        target: u32,
    },
    /// The connection failed: no privacy data, no capture.
    Unavailable,
}

/// The main thread's end for the visualizer: sends slot commands.
#[derive(Clone)]
pub(crate) struct Capture {
    /// Into the `pipewire` thread's loop.
    commands: pw::channel::Sender<CaptureCommand>,
}

impl std::fmt::Debug for Capture {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Capture").finish_non_exhaustive()
    }
}

impl Capture {
    /// Sends a command (never blocks).
    pub(crate) fn send(&self, command: CaptureCommand) {
        if self.commands.send(command).is_err() {
            eprintln!("stickBar: the PipeWire thread is gone");
        }
    }
}

/// Starts the `pipewire` thread. Its news arrives through the main loop at
/// `App::pipewire_event`. Returns the visualizer's command handle (`None` if
/// the thread couldn't start). If the connection fails, says so once: the
/// privacy box stays collapsed and the graphs go (C's "degrade gracefully").
pub(crate) fn start(handle: &LoopHandle<'static, App>) -> Option<Capture> {
    let (events, event_source) = channel::channel::<PipewireEvent>();
    let inserted = handle.insert_source(event_source, |event, (), app| {
        if let channel::Event::Msg(event) = event {
            app.pipewire_event(event);
        }
    });
    if let Err(err) = inserted {
        eprintln!(
            "stickBar: couldn't watch the PipeWire channel ({}) -- privacy indicator disabled",
            err.error
        );
        return None;
    }
    let (commands, command_queue) = pw::channel::channel::<CaptureCommand>();
    // Named, so it shows in `top -H` and the tests' thread checks. Started
    // after the signal setup in `main`, so it inherits the SIGUSR1 block, as
    // does any loop thread PipeWire starts from it.
    let started = std::thread::Builder::new()
        .name("pipewire".to_owned())
        .spawn(move || {
            if let Err(err) = run(events.clone(), command_queue) {
                eprintln!(
                    "stickBar: PipeWire unavailable ({err}) -- privacy indicator and media \
                     visualizer disabled for this run"
                );
                if events.send(PipewireEvent::Unavailable).is_err() {
                    eprintln!("stickBar: the main loop stopped listening");
                }
            }
        });
    match started {
        Ok(_detached) => Some(Capture { commands }),
        Err(err) => {
            eprintln!("stickBar: couldn't start the PipeWire thread ({err}) -- privacy disabled");
            None
        }
    }
}

/// One visualizer slot, on this thread.
#[derive(Default)]
struct Slot {
    /// (player pid, its rings) while it should capture.
    want: Option<(u32, SharedRings)>,
    /// Its live stream.
    stream: Option<capture::Capture>,
    /// What `stream` captures.
    target: Option<u32>,
}

/// The thread's state, shared by the registry, node and client listeners
/// and the command channel.
///
/// **Why `Rc<RefCell<..>>`:** pipewire-rs hands each event to its own
/// `'static` closure (registry `global`, `global_remove`, every bound node's
/// and client's `info`, the commands), and all of them update the same
/// state. They all run on this one thread's loop, one at a time, so a
/// `RefCell` is all the sharing needs (no lock); listeners hold a `Weak` so
/// nothing keeps it alive in a cycle.
struct Watch {
    /// What the indicator and the stream matching read.
    graph: graph::Graph,
    /// The bound streams and their info listeners, by node id: dropping an
    /// entry unbinds the node.
    nodes: HashMap<u32, (Node, NodeListener)>,
    /// The bound clients and their info listeners, by client id.
    clients: HashMap<u32, (Client, ClientListener)>,
    /// The connection, for creating capture streams.
    core: CoreRc,
    /// The visualizer's slots.
    slots: [Slot; MAX_SOURCES],
    /// The privacy state last sent.
    sent: PrivacyState,
    /// Where the main loop hears about it all.
    events: channel::Sender<PipewireEvent>,
}

impl Watch {
    /// Sends the privacy state, if it changed.
    fn publish(&mut self) {
        let state = self.graph.privacy_state();
        if state == self.sent {
            return;
        }
        self.sent = state.clone();
        self.send(PipewireEvent::Privacy(state));
    }

    /// Tells the main loop.
    fn send(&self, event: PipewireEvent) {
        if self.events.send(event).is_err() {
            eprintln!("stickBar: pipewire: the main loop stopped listening");
        }
    }

    /// Brings every slot's stream in line with what it wants (after a
    /// command, or when playback streams or client pids changed).
    fn reconcile(&mut self) {
        for index in 0..MAX_SOURCES {
            self.reconcile_slot(index);
        }
    }

    /// Brings slot `index`'s stream in line with what it wants: none when
    /// off; else its player's own playback stream (or the mix), recreated
    /// only when that target changed.
    fn reconcile_slot(&mut self, index: usize) {
        let Self {
            graph,
            core,
            slots,
            events,
            ..
        } = self;
        let Some(slot) = slots.get_mut(index) else {
            return;
        };
        let Some((pid, rings)) = slot.want.clone() else {
            slot.stream = None;
            slot.target = None;
            return;
        };
        let target = graph
            .find_playback(pid, capture::descends)
            .unwrap_or(capture::TARGET_MIX);
        if slot.stream.is_some() && slot.target == Some(target) {
            return;
        }
        // The old window isn't this slot's audio any more.
        slot.stream = None;
        if let Ok(mut r) = rings.lock() {
            r.clear();
        }
        match capture::create_stream(core, target, rings) {
            Ok(stream) => slot.stream = Some(stream),
            Err(err) => eprintln!("stickBar: visualizer capture stream failed: {err}"),
        }
        slot.target = Some(target);
        if events
            .send(PipewireEvent::Target {
                slot: index,
                target,
            })
            .is_err()
        {
            eprintln!("stickBar: pipewire: the main loop stopped listening");
        }
    }
}

/// Connects and follows the registry and the commands until the bar exits.
fn run(
    events: channel::Sender<PipewireEvent>,
    commands: pw::channel::Receiver<CaptureCommand>,
) -> Result<(), pw::Error> {
    pw::init();
    let main_loop = pw::main_loop::MainLoopRc::new(None)?;
    let context = pw::context::ContextRc::new(&main_loop, None)?;
    let core = context.connect_rc(None)?;
    let registry = core.get_registry_rc()?;

    let watch = Rc::new(RefCell::new(Watch {
        graph: graph::Graph::default(),
        nodes: HashMap::new(),
        clients: HashMap::new(),
        // Kept here: it's what capture streams are made on.
        core,
        slots: Default::default(),
        sent: PrivacyState::default(),
        events,
    }));
    let on_command = Rc::downgrade(&watch);
    let _commands = commands.attach(main_loop.loop_(), move |command| {
        let Some(shared) = on_command.upgrade() else {
            return;
        };
        let Ok(mut w) = shared.try_borrow_mut() else {
            return;
        };
        match command {
            CaptureCommand::Slot { slot, want } => {
                if let Some(s) = w.slots.get_mut(slot) {
                    s.want = want;
                }
                w.reconcile_slot(slot);
            }
        }
    });
    let registry_weak = registry.downgrade();
    let on_global = Rc::downgrade(&watch);
    let on_remove = Rc::downgrade(&watch);
    let _listener = registry
        .add_listener_local()
        .global(move |global| {
            let (Some(shared), Some(reg)) = (on_global.upgrade(), registry_weak.upgrade()) else {
                return;
            };
            if global.type_ == ObjectType::Node {
                add_node(&shared, &reg, global);
            } else if global.type_ == ObjectType::Link {
                add_link(&shared, global);
            } else if global.type_ == ObjectType::Client {
                add_client(&shared, &reg, global);
            }
        })
        .global_remove(move |id| {
            let Some(shared) = on_remove.upgrade() else {
                return;
            };
            let Ok(mut w) = shared.try_borrow_mut() else {
                return;
            };
            let removed = w.graph.nodes.remove(&id);
            let removed_link = w.graph.links.remove(&id).is_some();
            let removed_client = w.graph.clients.remove(&id).is_some();
            // Unbinds it, if it was bound.
            let _node = w.nodes.remove(&id);
            let _client = w.clients.remove(&id);
            if removed
                .as_ref()
                .is_some_and(|n| n.media_class == graph::PLAYBACK_CLASS)
            {
                w.reconcile();
            }
            if removed.is_some() || removed_link || removed_client {
                w.publish();
            }
        })
        .register();
    main_loop.run();
    Ok(())
}

/// A new node: its class, name, serial and client, and for a recording or
/// playback stream a binding whose info listener follows its running state
/// (a node merely existing isn't "in use": an app can hold a device open
/// while paused).
fn add_node(
    watch: &Rc<RefCell<Watch>>,
    registry: &pw::registry::RegistryRc,
    global: &GlobalObject<&DictRef>,
) {
    let props = global.props;
    let get = |key: &str| props.and_then(|p| p.get(key));
    let number = |key: &str| get(key).and_then(|v| v.parse::<u32>().ok()).unwrap_or(0);
    let media_class = get("media.class").unwrap_or_default().to_owned();
    let playback = media_class == graph::PLAYBACK_CLASS;
    let bind = playback || graph::is_watched(&media_class, get("node.name").unwrap_or_default());
    let node = graph::NodeEntry {
        media_class,
        name: graph::display_name(get),
        running: false,
        serial: number("object.serial"),
        client_id: number("client.id"),
    };
    let id = global.id;
    let Ok(mut w) = watch.try_borrow_mut() else {
        return;
    };
    let _old = w.graph.nodes.insert(id, node);
    if bind {
        match registry.bind::<Node, _>(global) {
            Ok(proxy) => {
                let weak = Rc::downgrade(watch);
                let listener = proxy
                    .add_listener_local()
                    .info(move |info| {
                        let Some(shared) = weak.upgrade() else {
                            return;
                        };
                        let Ok(mut state) = shared.try_borrow_mut() else {
                            return;
                        };
                        let running = matches!(info.state(), NodeState::Running);
                        let Some(entry) = state.graph.nodes.get_mut(&id) else {
                            return;
                        };
                        let changed = entry.running != running;
                        entry.running = running;
                        // A player's stream starting or stopping can change
                        // which one a graph should follow.
                        if changed && entry.media_class == graph::PLAYBACK_CLASS {
                            state.reconcile();
                        }
                        state.publish();
                    })
                    .register();
                let _replaced = w.nodes.insert(id, (proxy, listener));
            }
            Err(err) => eprintln!("stickBar: pipewire: couldn't watch node {id} ({err})"),
        }
    }
    if playback {
        w.reconcile();
    }
    w.publish();
}

/// A new link: its two ends, read straight off the global's props (no need
/// to bind it). A link can flip a video stream between webcam and screen
/// share, so the state is re-checked.
fn add_link(watch: &Rc<RefCell<Watch>>, global: &GlobalObject<&DictRef>) {
    let end = |key: &str| {
        global
            .props
            .and_then(|p| p.get(key))
            .and_then(|v| v.parse::<u32>().ok())
    };
    let (Some(output), Some(input)) = (end("link.output.node"), end("link.input.node")) else {
        return;
    };
    let Ok(mut w) = watch.try_borrow_mut() else {
        return;
    };
    let _old = w.graph.links.insert(global.id, (output, input));
    w.publish();
}

/// A new client: bound for its real process id. The registry global only
/// has `pipewire.sec.pid`, which for everything going through pipewire-pulse
/// (browsers, Steam, ...) is pipewire-pulse itself; the app's own pid,
/// `application.process.id`, is only in the bound client's info.
fn add_client(
    watch: &Rc<RefCell<Watch>>,
    registry: &pw::registry::RegistryRc,
    global: &GlobalObject<&DictRef>,
) {
    let id = global.id;
    let sec_pid = global
        .props
        .and_then(|p| p.get("pipewire.sec.pid"))
        .and_then(|v| v.parse::<u32>().ok())
        .unwrap_or(0);
    let Ok(mut w) = watch.try_borrow_mut() else {
        return;
    };
    let _old = w.graph.clients.insert(id, sec_pid);
    match registry.bind::<Client, _>(global) {
        Ok(proxy) => {
            let weak = Rc::downgrade(watch);
            let listener = proxy
                .add_listener_local()
                .info(move |info| {
                    let Some(pid) = info
                        .props()
                        .and_then(|p| p.get("application.process.id"))
                        .and_then(|v| v.parse::<u32>().ok())
                    else {
                        return;
                    };
                    let Some(shared) = weak.upgrade() else {
                        return;
                    };
                    let Ok(mut state) = shared.try_borrow_mut() else {
                        return;
                    };
                    let _previous = state.graph.clients.insert(id, pid);
                    // A stream may now match a player.
                    state.reconcile();
                })
                .register();
            let _replaced = w.clients.insert(id, (proxy, listener));
        }
        Err(err) => eprintln!("stickBar: pipewire: couldn't watch client {id} ({err})"),
    }
}
