//! `PipeWire`, for the privacy indicator (the C bar's `pipewire/`;
//! `docs/modules/privacy.md` has the details). libpulse, which the volume
//! and mic use, can't see the stream graph or video at all; `PipeWire` can.
//!
//! **A `pipewire` thread owns the connection** (pipewire-rs's main loop, the
//! C bar's `pw_thread_loop`). It follows the registry: every node's
//! `media.class` and name, every link's two ends, and the running state of
//! the recording streams (bound, with an info listener). After each change
//! it works out who's recording what ([`graph::Graph::privacy_state`]) and
//! sends that to the main loop on a calloop channel, **only if it differs
//! from the last one sent**. So the graph's constant churn (sinks, filters,
//! links, other apps' playback) costs the main thread nothing, as C's cached
//! pump did.

mod graph;

use std::{cell::RefCell, collections::HashMap, rc::Rc};

use calloop::{LoopHandle, channel};
use pipewire::{
    self as pw,
    node::{Node, NodeListener, NodeState},
    registry::GlobalObject,
    spa::utils::dict::DictRef,
    types::ObjectType,
};

pub(crate) use graph::{Category, PrivacyState};

use crate::wayland::App;

/// Starts the `pipewire` thread. Updates arrive through the main loop at
/// `App::privacy_update`. If anything fails, says so once: the privacy box
/// just stays collapsed for this run (C's "degrade gracefully").
pub(crate) fn start(handle: &LoopHandle<'static, App>) {
    let (updates, update_source) = channel::channel::<PrivacyState>();
    let inserted = handle.insert_source(update_source, |event, (), app| {
        if let channel::Event::Msg(state) = event {
            app.privacy_update(state);
        }
    });
    if let Err(err) = inserted {
        eprintln!(
            "stickBar: couldn't watch the PipeWire channel ({}) -- privacy indicator disabled",
            err.error
        );
        return;
    }
    // Named, so it shows in `top -H` and the tests' thread checks. Started
    // after the signal setup in `main`, so it inherits the SIGUSR1 block, as
    // does any loop thread PipeWire starts from it.
    let started = std::thread::Builder::new()
        .name("pipewire".to_owned())
        .spawn(move || {
            if let Err(err) = run(updates) {
                eprintln!(
                    "stickBar: PipeWire unavailable ({err}) -- privacy indicator disabled for \
                     this run"
                );
            }
        });
    if let Err(err) = started {
        eprintln!("stickBar: couldn't start the PipeWire thread ({err}) -- privacy disabled");
    }
}

/// The thread's state, shared by the registry and node listeners.
///
/// **Why `Rc<RefCell<..>>`:** pipewire-rs hands each event to its own
/// `'static` closure (registry `global`, `global_remove`, and every bound
/// node's `info`), and all of them update the same graph. They all run on
/// this one thread's loop, one at a time, so a `RefCell` is all the sharing
/// needs (no lock); node listeners hold a `Weak` so nothing keeps it alive
/// in a cycle.
struct Watch {
    /// What the indicator reads.
    graph: graph::Graph,
    /// The bound recording streams and their info listeners, by node id:
    /// dropping an entry unbinds the node.
    proxies: HashMap<u32, (Node, NodeListener)>,
    /// The state last sent.
    sent: PrivacyState,
    /// Where the main loop hears about it.
    updates: channel::Sender<PrivacyState>,
}

impl Watch {
    /// Sends the current state, if it changed.
    fn publish(&mut self) {
        let state = self.graph.privacy_state();
        if state == self.sent {
            return;
        }
        self.sent = state.clone();
        if self.updates.send(state).is_err() {
            eprintln!("stickBar: privacy: the main loop stopped listening");
        }
    }
}

/// Connects and follows the registry until the bar exits.
fn run(updates: channel::Sender<PrivacyState>) -> Result<(), pw::Error> {
    pw::init();
    let main_loop = pw::main_loop::MainLoopRc::new(None)?;
    let context = pw::context::ContextRc::new(&main_loop, None)?;
    let core = context.connect_rc(None)?;
    let registry = core.get_registry_rc()?;

    let watch = Rc::new(RefCell::new(Watch {
        graph: graph::Graph::default(),
        proxies: HashMap::new(),
        sent: PrivacyState::default(),
        updates,
    }));
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
            }
        })
        .global_remove(move |id| {
            let Some(shared) = on_remove.upgrade() else {
                return;
            };
            let Ok(mut w) = shared.try_borrow_mut() else {
                return;
            };
            let removed_node = w.graph.nodes.remove(&id).is_some();
            let removed_link = w.graph.links.remove(&id).is_some();
            // Unbinds it, if it was a watched stream.
            let _proxy = w.proxies.remove(&id);
            if removed_node || removed_link {
                w.publish();
            }
        })
        .register();
    main_loop.run();
    Ok(())
}

/// A new node: its class and name, and for a recording stream a binding
/// whose info listener follows its running state (a node merely existing
/// isn't "in use": an app can hold a device open while paused).
fn add_node(
    watch: &Rc<RefCell<Watch>>,
    registry: &pw::registry::RegistryRc,
    global: &GlobalObject<&DictRef>,
) {
    let props = global.props;
    let get = |key: &str| props.and_then(|p| p.get(key));
    let media_class = get("media.class").unwrap_or_default().to_owned();
    let watched = graph::is_watched(&media_class, get("node.name").unwrap_or_default());
    let node = graph::NodeEntry {
        media_class,
        name: graph::display_name(get),
        running: false,
    };
    let id = global.id;
    let Ok(mut w) = watch.try_borrow_mut() else {
        return;
    };
    let _old = w.graph.nodes.insert(id, node);
    if watched {
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
                        if let Some(entry) = state.graph.nodes.get_mut(&id) {
                            entry.running = matches!(info.state(), NodeState::Running);
                        }
                        state.publish();
                    })
                    .register();
                let _replaced = w.proxies.insert(id, (proxy, listener));
            }
            Err(err) => eprintln!("stickBar: privacy: couldn't watch node {id} ({err})"),
        }
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
