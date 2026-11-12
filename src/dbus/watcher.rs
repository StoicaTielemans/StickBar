//! Becoming the `StatusNotifierWatcher`, or a Host of someone else's (the C
//! bar's `dbus/sni_watcher_service.c` and `dbus/sni_watcher.c`). The bar tries
//! to own `org.kde.StatusNotifierWatcher` itself first, so the tray works with
//! no other Watcher running; if something else already owns it, the bar
//! registers as a Host with that one and follows its signals. Nothing else
//! cares which: both end in `Registry::add` / `remove`.
//!
//! **The Watcher object is served by hand**, from a stream of the method calls
//! to its path, not with zbus's `#[interface]` macro: the macro's generated
//! items would fight the lint policy, and zbus only starts its own object
//! server when something is served through it (checked in zbus 5's
//! `connection/builder.rs`), so it never answers these calls a second time.

use std::collections::HashMap;

use zbus::{
    Connection, MatchRule, Message, MessageStream,
    fdo::{RequestNameFlags, RequestNameReply},
    message::{Flags, Type},
    zvariant::{OwnedValue, Value},
};

use super::{sni::Registry, stream};

/// The interface every Watcher implements, whichever bus name owns it (a
/// quirk of the SNI ecosystem: the interface kept its KDE name).
const WATCHER_INTERFACE: &str = "org.kde.StatusNotifierWatcher";
/// The Watcher's object path.
const WATCHER_PATH: &str = "/StatusNotifierWatcher";
/// The bus name nearly every item looks for.
const NAME_KDE: &str = "org.kde.StatusNotifierWatcher";
/// The freedesktop name a few hosts hard-code instead.
const NAME_FDO: &str = "org.freedesktop.StatusNotifierWatcher";
/// An item's object path when it registers with just a bus name.
const DEFAULT_ITEM_PATH: &str = "/StatusNotifierItem";
/// The properties interface.
const PROPERTIES: &str = "org.freedesktop.DBus.Properties";
/// What `Introspect` answers, so `busctl introspect` shows the object.
const INTROSPECTION: &str = r#"<!DOCTYPE node PUBLIC "-//freedesktop//DTD D-BUS Object Introspection 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd">
<node>
 <interface name="org.kde.StatusNotifierWatcher">
  <method name="RegisterStatusNotifierItem"><arg type="s" direction="in"/></method>
  <method name="RegisterStatusNotifierHost"><arg type="s" direction="in"/></method>
  <property name="RegisteredStatusNotifierItems" type="as" access="read"/>
  <property name="IsStatusNotifierHostRegistered" type="b" access="read"/>
  <property name="ProtocolVersion" type="i" access="read"/>
  <signal name="StatusNotifierItemRegistered"><arg type="s"/></signal>
  <signal name="StatusNotifierItemUnregistered"><arg type="s"/></signal>
  <signal name="StatusNotifierHostRegistered"/>
 </interface>
</node>
"#;

/// Which role the bar ended up in.
pub(crate) enum Mode {
    /// The bar is the Watcher: the method calls to its object.
    Watcher(MessageStream),
    /// Another process is the Watcher.
    Host {
        /// Its Registered/Unregistered signals.
        signals: MessageStream,
        /// The items it already had (`bus_name` + `path` each).
        items: Vec<String>,
    },
    /// Neither worked: the tray stays empty.
    None,
}

/// Claims the Watcher name, or falls back to registering as a Host.
pub(crate) async fn start(conn: &Connection) -> zbus::Result<Mode> {
    // Subscribed first, so an item that registers the moment the name is
    // ours isn't missed.
    let calls = stream(
        conn,
        MatchRule::builder()
            .msg_type(Type::MethodCall)
            .path(WATCHER_PATH)?
            .build(),
    )
    .await?;
    // DoNotQueue: fail at once if someone else owns it (the user's own bar,
    // usually), instead of waiting in line for it.
    let claimed = conn
        .request_name_with_flags(NAME_KDE, RequestNameFlags::DoNotQueue.into())
        .await;
    if let Ok(RequestNameReply::PrimaryOwner | RequestNameReply::AlreadyOwner) = claimed {
        // Best effort: the KDE name is what matters.
        if let Err(err) = conn
            .request_name_with_flags(NAME_FDO, RequestNameFlags::DoNotQueue.into())
            .await
        {
            eprintln!("stickBar: tray: couldn't also claim {NAME_FDO} ({err})");
        }
        return Ok(Mode::Watcher(calls));
    }
    drop(calls);
    host(conn).await
}

/// Registers as a Host with the existing Watcher and reads its items.
async fn host(conn: &Connection) -> zbus::Result<Mode> {
    let signals = stream(
        conn,
        MatchRule::builder()
            .msg_type(Type::Signal)
            .interface(WATCHER_INTERFACE)?
            .path(WATCHER_PATH)?
            .build(),
    )
    .await?;
    let secs = std::time::SystemTime::now()
        .duration_since(std::time::SystemTime::UNIX_EPOCH)
        .map_or(0, |d| d.as_secs());
    let host_name = format!(
        "org.freedesktop.StatusNotifierHost-{}-{secs}",
        std::process::id()
    );
    // Only needs to be unique enough to mean something to the Watcher.
    if let Err(err) = conn.request_name(host_name.as_str()).await {
        eprintln!("stickBar: tray: couldn't claim {host_name} ({err})");
    }
    let mut watcher = None;
    for name in [NAME_KDE, NAME_FDO] {
        let registered = conn
            .call_method(
                Some(name),
                WATCHER_PATH,
                Some(WATCHER_INTERFACE),
                "RegisterStatusNotifierHost",
                &host_name,
            )
            .await;
        if registered.is_ok() {
            watcher = Some(name);
            break;
        }
    }
    let Some(watcher) = watcher else {
        eprintln!(
            "stickBar: no StatusNotifierWatcher found (tried {NAME_KDE}, {NAME_FDO}) -- \
             system tray will stay empty for this run"
        );
        return Ok(Mode::None);
    };
    let items = match conn
        .call_method(
            Some(watcher),
            WATCHER_PATH,
            Some(PROPERTIES),
            "Get",
            &(WATCHER_INTERFACE, "RegisteredStatusNotifierItems"),
        )
        .await
        .and_then(|reply| reply.body().deserialize::<OwnedValue>())
        .and_then(|value| Ok(Vec::<String>::try_from(value)?))
    {
        Ok(items) => items,
        Err(err) => {
            eprintln!("stickBar: failed to fetch the initial tray item list ({err})");
            Vec::new()
        }
    };
    Ok(Mode::Host { signals, items })
}

/// Splits `":1.30/StatusNotifierItem"` into its bus name and path at the first
/// `/` (bus names never contain one); with no `/` the whole string is the bus
/// name and the path is the default. Both forms occur on real items.
pub(crate) fn split_bus_name_path(entry: &str) -> (String, String) {
    match entry.find('/') {
        Some(slash) => (
            entry.get(..slash).unwrap_or_default().to_owned(),
            entry.get(slash..).unwrap_or_default().to_owned(),
        ),
        None => (entry.to_owned(), DEFAULT_ITEM_PATH.to_owned()),
    }
}

/// Host mode: a signal from the Watcher.
pub(crate) async fn handle_signal(msg: &Message, tray: &mut Registry) {
    let header = msg.header();
    let Ok(entry) = msg.body().deserialize::<String>() else {
        return;
    };
    let (bus_name, path) = split_bus_name_path(&entry);
    match header.member().map(zbus::names::MemberName::as_str) {
        Some("StatusNotifierItemRegistered") => tray.add(&bus_name, &path).await,
        Some("StatusNotifierItemUnregistered") => tray.remove(&bus_name, &path),
        _ => {}
    }
}

/// Watcher mode: a method call to the Watcher object.
pub(crate) async fn handle_call(conn: &Connection, msg: &Message, tray: &mut Registry) {
    let header = msg.header();
    let wants_reply = !header.primary().flags().contains(Flags::NoReplyExpected);
    let interface = header.interface().map(|i| i.as_str().to_owned());
    let member = header.member().map(|m| m.as_str().to_owned());
    let result = match (interface.as_deref(), member.as_deref()) {
        (Some(WATCHER_INTERFACE), Some("RegisterStatusNotifierItem")) => {
            let Ok(service) = msg.body().deserialize::<String>() else {
                return reply_error(conn, msg, "InvalidArgs", "expected a string").await;
            };
            // Two real conventions: Steam passes its object path (the bus
            // name is then the caller's own), KDE apps pass a bus name (the
            // path is then the default).
            let (bus_name, path) = if service.starts_with('/') {
                let sender = header.sender().map(|s| s.as_str().to_owned());
                (sender.unwrap_or_default(), service)
            } else {
                (service, DEFAULT_ITEM_PATH.to_owned())
            };
            // Answered before the item is read: an app that waits for this
            // reply before serving its own properties (a blocking client)
            // would otherwise deadlock with the bar until the timeout.
            let replied = reply(conn, msg, wants_reply, &()).await;
            tray.add(&bus_name, &path).await;
            let entry = format!("{bus_name}{path}");
            replied.and(emit(conn, "StatusNotifierItemRegistered", &entry).await)
        }
        (Some(WATCHER_INTERFACE), Some("RegisterStatusNotifierHost")) => {
            // The host's name isn't kept: the bar always hosts its own items.
            let replied = reply(conn, msg, wants_reply, &()).await;
            replied.and(emit(conn, "StatusNotifierHostRegistered", &()).await)
        }
        (Some(PROPERTIES), Some("Get")) => match msg.body().deserialize::<(String, String)>() {
            Ok((_iface, name)) => match property(tray, &name) {
                Some(value) => reply(conn, msg, wants_reply, &value).await,
                None => {
                    return reply_error(conn, msg, "UnknownProperty", "no such property").await;
                }
            },
            Err(_) => return reply_error(conn, msg, "InvalidArgs", "expected (ss)").await,
        },
        (Some(PROPERTIES), Some("GetAll")) => {
            let all: HashMap<&str, Value<'_>> = PROPERTY_NAMES
                .iter()
                .filter_map(|&name| property(tray, name).map(|v| (name, v)))
                .collect();
            reply(conn, msg, wants_reply, &all).await
        }
        (Some("org.freedesktop.DBus.Introspectable"), Some("Introspect")) => {
            reply(conn, msg, wants_reply, &INTROSPECTION).await
        }
        (Some("org.freedesktop.DBus.Peer"), Some("Ping")) => {
            reply(conn, msg, wants_reply, &()).await
        }
        _ => return reply_error(conn, msg, "UnknownMethod", "no such method").await,
    };
    if let Err(err) = result {
        eprintln!("stickBar: tray: answering a Watcher call failed ({err})");
    }
}

/// The Watcher's properties.
const PROPERTY_NAMES: [&str; 3] = [
    "RegisteredStatusNotifierItems",
    "IsStatusNotifierHostRegistered",
    "ProtocolVersion",
];

/// One Watcher property's value.
fn property(tray: &Registry, name: &str) -> Option<Value<'static>> {
    match name {
        "RegisteredStatusNotifierItems" => Some(Value::from(tray.entries())),
        // Always true: the bar hosts its own items.
        "IsStatusNotifierHostRegistered" => Some(Value::from(true)),
        "ProtocolVersion" => Some(Value::from(0_i32)),
        _ => None,
    }
}

/// Replies to `msg` with `body`, unless the caller asked for no reply.
async fn reply<B>(conn: &Connection, msg: &Message, wanted: bool, body: &B) -> zbus::Result<()>
where
    B: zbus::export::serde::Serialize + zbus::zvariant::DynamicType,
{
    if !wanted {
        return Ok(());
    }
    conn.reply(&msg.header(), body).await
}

/// Replies to `msg` with `org.freedesktop.DBus.Error.<name>`.
async fn reply_error(conn: &Connection, msg: &Message, name: &str, text: &str) {
    let error = format!("org.freedesktop.DBus.Error.{name}");
    if let Err(err) = conn.reply_error(&msg.header(), error.as_str(), &text).await {
        eprintln!("stickBar: tray: answering a Watcher call failed ({err})");
    }
}

/// Emits a Watcher signal.
async fn emit<B>(conn: &Connection, name: &str, body: &B) -> zbus::Result<()>
where
    B: zbus::export::serde::Serialize + zbus::zvariant::DynamicType,
{
    conn.emit_signal(None::<&str>, WATCHER_PATH, WATCHER_INTERFACE, name, body)
        .await
}

/// Watcher mode: tells other hosts an item went away (C never did, which left
/// hosts of the C bar with stale icons).
pub(crate) async fn emit_unregistered(conn: &Connection, entry: &str) {
    if let Err(err) = emit(conn, "StatusNotifierItemUnregistered", &entry).await {
        eprintln!("stickBar: tray: emitting StatusNotifierItemUnregistered failed ({err})");
    }
}

#[cfg(test)]
mod tests {
    use super::split_bus_name_path;

    #[test]
    fn splits_entries() {
        assert_eq!(
            split_bus_name_path(":1.30/StatusNotifierItem"),
            (":1.30".to_owned(), "/StatusNotifierItem".to_owned())
        );
        assert_eq!(
            split_bus_name_path(":1.52/org/ayatana/NotificationItem/steam"),
            (
                ":1.52".to_owned(),
                "/org/ayatana/NotificationItem/steam".to_owned()
            )
        );
        assert_eq!(
            split_bus_name_path("org.kde.StatusNotifierItem-1-1"),
            (
                "org.kde.StatusNotifierItem-1-1".to_owned(),
                "/StatusNotifierItem".to_owned()
            )
        );
    }
}
