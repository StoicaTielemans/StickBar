//! The pulse thread: owns the libpulse mainloop, the connection and the
//! device cache (the C bar's `pulse_client.c`, without its shared lock; see
//! `mod.rs` for the thread design).
//!
//! Its loop: block in `iterate` until the server or the main thread has
//! something; run libpulse's callbacks (they update the cache); run queued
//! commands; then, if the cache changed, send the main thread one snapshot.
//! One snapshot per iteration batches the per-item list callbacks (one
//! callback per sink) into a single update.
//!
//! The cache is an `Rc<RefCell<…>>`: the callbacks are `'static` closures
//! that each keep a handle on it. `Rc`/`RefCell` are the single-threaded
//! versions of `Arc`/`Mutex`; the compiler won't let them leave this thread,
//! which is the guarantee that nothing else touches the cache.

use std::{
    cell::RefCell,
    io::Read,
    os::{fd::AsRawFd, unix::net::UnixStream},
    rc::Rc,
    sync::mpsc,
};

use calloop::channel;
use libpulse_binding::{
    callbacks::ListResult,
    context::{
        Context, FlagSet as ContextFlags, State,
        introspect::{Introspector, ServerInfo, SinkInfo, SinkInputInfo, SourceInfo},
        subscribe::{Facility, InterestMaskSet, Operation},
    },
    def::{SinkState, SourceState},
    mainloop::{
        api::Mainloop as _,
        events::io::FlagSet as IoFlags,
        standard::{IterateResult, Mainloop},
    },
    operation::Operation as Request,
    volume::{ChannelVolumes, Volume},
};

use super::{
    AudioState, Command, Kind, MAX_VOLUME, Node, Stream, percent_to_raw, raw_to_percent, stepped,
};

/// The cache, shared between this thread's callbacks.
type Shared = Rc<RefCell<Cache>>;

/// What the server has told us, and whether that's changed since the last
/// snapshot went out.
#[derive(Debug, Default)]
struct Cache {
    /// The current state.
    state: AudioState,
    /// Changed since the last snapshot.
    dirty: bool,
}

impl Cache {
    /// The sinks or the sources.
    const fn nodes(&mut self, kind: Kind) -> &mut Vec<Node> {
        match kind {
            Kind::Sink => &mut self.state.sinks,
            Kind::Source => &mut self.state.sources,
        }
    }

    /// Adds `node`, or updates the node with its index.
    fn upsert(&mut self, kind: Kind, node: Node) {
        let nodes = self.nodes(kind);
        match nodes.iter_mut().find(|n| n.index == node.index) {
            Some(existing) => *existing = node,
            None => nodes.push(node),
        }
        self.dirty = true;
    }

    /// Removes the node with `index`.
    fn remove(&mut self, kind: Kind, index: u32) {
        self.nodes(kind).retain(|n| n.index != index);
        self.dirty = true;
    }

    /// Adds `stream`, or updates the stream with its index.
    fn upsert_stream(&mut self, stream: Stream) {
        let streams = &mut self.state.streams;
        match streams.iter_mut().find(|s| s.index == stream.index) {
            Some(existing) => *existing = stream,
            None => streams.push(stream),
        }
        self.dirty = true;
    }

    /// Removes the stream with `index`.
    fn remove_stream(&mut self, index: u32) {
        self.state.streams.retain(|s| s.index != index);
        self.dirty = true;
    }

    /// The snapshot to send, if anything changed since the last one.
    fn take_snapshot(&mut self) -> Option<AudioState> {
        self.dirty.then(|| {
            self.dirty = false;
            self.state.clone()
        })
    }
}

/// The thread's body. Reports the connection result on `ready`, then serves
/// until the server goes away or the main thread does.
pub(super) fn run(
    updates: &channel::Sender<AudioState>,
    ready: &mpsc::Sender<Result<(), String>>,
    commands: &mpsc::Receiver<Command>,
    wake: &UnixStream,
) {
    // Declared first, so dropped last: the context and events below refer to it.
    let (mut mainloop, mut context) = match connect() {
        Ok(connection) => connection,
        Err(err) => {
            let _unheard = ready.send(Err(err));
            return;
        }
    };

    // The main thread writes a byte to `wake` after queueing a command. This
    // makes that fd one of the things `iterate` waits on; the callback only
    // empties the socket (the commands are run below, after `iterate`).
    let Ok(wake_reader) = wake.try_clone() else {
        let _unheard = ready.send(Err("couldn't clone the wake socket".to_owned()));
        return;
    };
    let wake_event = mainloop.new_io_event(
        wake.as_raw_fd(),
        IoFlags::INPUT,
        Box::new(move |_, _, _| {
            let mut buf = [0_u8; 64];
            while matches!((&wake_reader).read(&mut buf), Ok(n) if n > 0) {}
        }),
    );
    if wake_event.is_none() {
        let _unheard = ready.send(Err("couldn't watch the wake socket".to_owned()));
        return;
    }

    let cache = Shared::default();
    watch(&mut context, &cache);
    let mut introspector = context.introspect();
    let _unheard = ready.send(Ok(()));

    loop {
        if !matches!(mainloop.iterate(true), IterateResult::Success(_)) {
            break;
        }
        if !context.get_state().is_good() {
            eprintln!("stickBar: lost the PulseAudio connection -- volume/mic stop updating");
            break;
        }
        while let Ok(command) = commands.try_recv() {
            run_command(
                &mut context,
                &mut introspector,
                &cache.borrow().state,
                &command,
            );
        }
        let snapshot = cache.borrow_mut().take_snapshot();
        if let Some(snapshot) = snapshot
            && updates.send(snapshot).is_err()
        {
            break; // the main thread is gone
        }
    }
}

/// Creates the mainloop and connects, iterating until the connection is
/// ready or has failed.
fn connect() -> Result<(Mainloop, Context), String> {
    let mut mainloop = Mainloop::new().ok_or("couldn't create a libpulse mainloop")?;
    let mut context =
        Context::new(&mainloop, "stickBar").ok_or("couldn't create a libpulse context")?;
    context
        .connect(None, ContextFlags::NOFLAGS, None)
        .map_err(|err| format!("couldn't connect to the PulseAudio-compatible server: {err:?}"))?;
    loop {
        if !matches!(mainloop.iterate(true), IterateResult::Success(_)) {
            return Err("the libpulse mainloop failed while connecting".to_owned());
        }
        match context.get_state() {
            State::Ready => return Ok((mainloop, context)),
            State::Failed | State::Terminated => {
                return Err("failed to connect to the PulseAudio-compatible server".to_owned());
            }
            State::Unconnected | State::Connecting | State::Authorizing | State::SettingName => {}
        }
    }
}

/// Subscribes to device, stream and server changes, and fetches everything
/// once (the C bar's `context_state_cb` READY branch).
fn watch(context: &mut Context, cache: &Shared) {
    let introspector = context.introspect();
    let events_cache = Rc::clone(cache);
    context.set_subscribe_callback(Some(Box::new(move |facility, operation, index| {
        let removed = operation == Some(Operation::Removed);
        let shared = &events_cache;
        // A new or changed object is fetched again; a removed one dropped.
        // SERVER events mean a default device changed.
        match facility {
            Some(Facility::Sink) if removed => shared.borrow_mut().remove(Kind::Sink, index),
            Some(Facility::Sink) => {
                started(introspector.get_sink_info_by_index(index, on_sink(shared)));
            }
            Some(Facility::Source) if removed => shared.borrow_mut().remove(Kind::Source, index),
            Some(Facility::Source) => {
                started(introspector.get_source_info_by_index(index, on_source(shared)));
            }
            Some(Facility::SinkInput) if removed => shared.borrow_mut().remove_stream(index),
            Some(Facility::SinkInput) => {
                started(introspector.get_sink_input_info(index, on_stream(shared)));
            }
            Some(Facility::Server) => {
                started(introspector.get_server_info(on_server(shared)));
            }
            _ => {} // cards, modules, clients...: not tracked
        }
    })));
    started(context.subscribe(
        InterestMaskSet::SINK
            | InterestMaskSet::SOURCE
            | InterestMaskSet::SINK_INPUT
            | InterestMaskSet::SERVER,
        |_success| {},
    ));
    let fetcher = context.introspect();
    started(fetcher.get_sink_info_list(on_sink(cache)));
    started(fetcher.get_sink_input_info_list(on_stream(cache)));
    started(fetcher.get_source_info_list(on_source(cache)));
    started(fetcher.get_server_info(on_server(cache)));
}

/// The callback for sink info: caches each sink.
fn on_sink(cache: &Shared) -> impl FnMut(ListResult<&SinkInfo<'_>>) + 'static {
    let cache = Rc::clone(cache);
    move |result| {
        if let ListResult::Item(info) = result {
            let node = Node {
                index: info.index,
                name: text(info.name.as_deref()),
                description: text(info.description.as_deref().or(info.name.as_deref())),
                volume: raw_to_percent(info.volume.avg().0),
                muted: info.mute,
                channels: info.volume.len(),
                state: sink_state(info.state),
                bus: bus(info.proplist.get_str("device.bus")),
                rate: info.sample_spec.rate,
                monitor: false,
            };
            cache.borrow_mut().upsert(Kind::Sink, node);
        }
    }
}

/// The callback for source info: caches each source.
fn on_source(cache: &Shared) -> impl FnMut(ListResult<&SourceInfo<'_>>) + 'static {
    let cache = Rc::clone(cache);
    move |result| {
        if let ListResult::Item(info) = result {
            let node = Node {
                index: info.index,
                name: text(info.name.as_deref()),
                description: text(info.description.as_deref().or(info.name.as_deref())),
                volume: raw_to_percent(info.volume.avg().0),
                muted: info.mute,
                channels: info.volume.len(),
                state: source_state(info.state),
                bus: bus(info.proplist.get_str("device.bus")),
                rate: info.sample_spec.rate,
                monitor: info.monitor_of_sink.is_some(),
            };
            cache.borrow_mut().upsert(Kind::Source, node);
        }
    }
}

/// The callback for sink-input info: caches each app stream, named by
/// `application.name`, else its own name.
fn on_stream(cache: &Shared) -> impl FnMut(ListResult<&SinkInputInfo<'_>>) + 'static {
    let cache = Rc::clone(cache);
    move |result| {
        if let ListResult::Item(info) = result {
            let name = info
                .proplist
                .get_str("application.name")
                .filter(|name| !name.is_empty())
                .or_else(|| info.name.as_deref().map(str::to_owned))
                .unwrap_or_else(|| "Unknown app".to_owned());
            let stream = Stream {
                index: info.index,
                name,
                volume: raw_to_percent(info.volume.avg().0),
                muted: info.mute,
                channels: info.volume.len(),
            };
            cache.borrow_mut().upsert_stream(stream);
        }
    }
}

/// The callback for server info: the default sink's and source's names.
fn on_server(cache: &Shared) -> impl FnMut(&ServerInfo<'_>) + 'static {
    let cache = Rc::clone(cache);
    move |info| {
        let mut cache = cache.borrow_mut();
        cache.state.default_sink = text(info.default_sink_name.as_deref());
        cache.state.default_source = text(info.default_source_name.as_deref());
        cache.dirty = true;
    }
}

/// Runs one command against the server. The results come back as
/// subscription events, like any other change.
fn run_command(
    context: &mut Context,
    introspector: &mut Introspector,
    state: &AudioState,
    command: &Command,
) {
    // Every command but `SetDefault` acts on one device: resolve it, and
    // what to do with it, first.
    let (kind, index, action) = match *command {
        Command::SetDefault { kind, ref name } => {
            started(match kind {
                Kind::Sink => context.set_default_sink(name, |_success| {}),
                Kind::Source => context.set_default_source(name, |_success| {}),
            });
            return;
        }
        Command::AdjustDefault { kind, delta } => match state.default_node(kind) {
            Some(node) => (
                kind,
                node.index,
                Action::Volume(stepped(node.volume, delta)),
            ),
            None => return,
        },
        Command::ToggleDefaultMute(kind) => match state.default_node(kind) {
            Some(node) => (kind, node.index, Action::Mute(!node.muted)),
            None => return,
        },
        Command::SetVolume {
            kind,
            index,
            percent,
        } => (kind, index, Action::Volume(percent.min(MAX_VOLUME))),
        Command::AdjustVolume { kind, index, delta } => match state.node(kind, index) {
            Some(node) => (kind, index, Action::Volume(stepped(node.volume, delta))),
            None => return,
        },
        Command::ToggleMute { kind, index } => match state.node(kind, index) {
            Some(node) => (kind, index, Action::Mute(!node.muted)),
            None => return,
        },
    };
    // A device gone since the command was sent is skipped.
    let Some(node) = state.node(kind, index) else {
        return;
    };
    started(match (kind, action) {
        (Kind::Sink, Action::Volume(percent)) => {
            introspector.set_sink_volume_by_index(index, &volumes(node.channels, percent), None)
        }
        (Kind::Source, Action::Volume(percent)) => {
            introspector.set_source_volume_by_index(index, &volumes(node.channels, percent), None)
        }
        (Kind::Sink, Action::Mute(mute)) => introspector.set_sink_mute_by_index(index, mute, None),
        (Kind::Source, Action::Mute(mute)) => {
            introspector.set_source_mute_by_index(index, mute, None)
        }
    });
}

/// What to set on a device.
#[derive(Clone, Copy, Debug)]
#[expect(
    variant_size_differences,
    reason = "a short-lived local value, never stored or sent; 4 bytes don't matter"
)]
enum Action {
    /// This volume, in %.
    Volume(u32),
    /// This mute state.
    Mute(bool),
}

/// Lets a request run without waiting on it. libpulse returns a handle per
/// request; dropping it only releases the handle, the request still runs and
/// its callback still fires (results arrive as events anyway).
fn started<T: ?Sized>(_request: Request<T>) {}

/// Every channel at `percent`, on `channels` channels (2 if never seen).
fn volumes(channels: u8, percent: u32) -> ChannelVolumes {
    let mut volume = ChannelVolumes::default();
    // `set` fills the channels and hands back the result.
    *volume.set(
        if channels == 0 { 2 } else { channels },
        Volume(percent_to_raw(percent)),
    )
}

/// An optional server string as an owned one ("" if missing).
fn text(value: Option<&str>) -> String {
    value.unwrap_or_default().to_owned()
}

/// The `device.bus` property for display: `usb` → `USB`.
fn bus(value: Option<String>) -> String {
    value.unwrap_or_default().to_uppercase()
}

/// A sink's state as the dropdown's word.
const fn sink_state(state: SinkState) -> &'static str {
    match state {
        SinkState::Running => "running",
        SinkState::Idle => "idle",
        SinkState::Suspended => "suspended",
        SinkState::Invalid => "",
    }
}

/// A source's state as the dropdown's word (same values as sinks').
const fn source_state(state: SourceState) -> &'static str {
    match state {
        SourceState::Running => "running",
        SourceState::Idle => "idle",
        SourceState::Suspended => "suspended",
        SourceState::Invalid => "",
    }
}
