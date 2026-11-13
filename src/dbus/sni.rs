//! The tray's items on the `dbus` thread (the C bar's `dbus/sni_registry.c`):
//! what each one is called, where its menu lives, and its icon. The main
//! thread only sees the ids handed out here, in [`TrayEvent`]s.

use std::{collections::HashMap, fmt};

use calloop::channel;
use zbus::{
    Connection, Message,
    export::serde::de,
    message::Flags,
    zvariant::{OwnedValue, Signature, Type, as_value},
};

use super::{Icon, TrayCommand, TrayEvent, dbusmenu, icon_theme, sni_icon, text_of};
use sni_icon::Pixmap;

/// Every item's interface.
pub(crate) const ITEM_INTERFACE: &str = "org.kde.StatusNotifierItem";

/// One tracked item.
#[derive(Debug)]
struct Item {
    /// The id the main thread knows it by.
    id: u64,
    /// The bus name it was registered under (well-known or unique).
    bus_name: String,
    /// Its object path.
    path: String,
    /// The unique name owning `bus_name` when it was added. Signals carry the
    /// unique name as their sender, so `NewIcon` is matched against this.
    owner: String,
    /// Its menu's object path; "" or "/" means no menu.
    menu_path: String,
}

impl Item {
    /// `bus_name` + `path`, the form `RegisteredStatusNotifierItems` uses.
    fn entry(&self) -> String {
        format!("{}{}", self.bus_name, self.path)
    }

    /// Whether it has a menu. SNI has no null object path: "/" is the
    /// conventional "none", and "" covers items that leave `Menu` out.
    fn has_menu(&self) -> bool {
        !self.menu_path.is_empty() && self.menu_path != "/"
    }
}

/// Every item, in registration order.
pub(crate) struct Registry {
    /// The session bus.
    conn: Connection,
    /// Where the main thread hears about changes.
    events: channel::Sender<TrayEvent>,
    /// The items.
    items: Vec<Item>,
    /// The next item's id.
    next_id: u64,
}

impl Registry {
    /// An empty registry.
    pub(crate) const fn new(conn: Connection, events: channel::Sender<TrayEvent>) -> Self {
        Self {
            conn,
            events,
            items: Vec::new(),
            next_id: 0,
        }
    }

    /// Every item as `bus_name` + `path` (the Watcher's
    /// `RegisteredStatusNotifierItems`).
    pub(crate) fn entries(&self) -> Vec<String> {
        self.items.iter().map(Item::entry).collect()
    }

    /// Tells the main thread.
    fn send(&self, event: TrayEvent) {
        if self.events.send(event).is_err() {
            eprintln!("stickBar: tray: the main loop stopped listening");
        }
    }

    /// Adds the item at (`bus_name`, `path`): reads its properties (Id, Title,
    /// Menu, the icon) with one `GetAll`, then tells the main thread. A
    /// failed read still adds it, with a placeholder icon, as in C. No-op if
    /// it's already tracked.
    pub(crate) async fn add(&mut self, bus_name: &str, path: &str) {
        if self
            .items
            .iter()
            .any(|i| i.bus_name == bus_name && i.path == path)
        {
            return;
        }
        let owner = if bus_name.starts_with(':') {
            bus_name.to_owned()
        } else {
            self.name_owner(bus_name).await.unwrap_or_default()
        };
        let props = match self.get_all(bus_name, path).await {
            Ok(props) => props,
            Err(err) => {
                eprintln!(
                    "stickBar: failed to fetch tray item properties for {bus_name}{path} ({err})"
                );
                ItemProps::default()
            }
        };
        let title = if props.title.is_empty() {
            props.id
        } else {
            props.title
        };
        // No usable IconPixmap: the item's IconName as a PNG, if one is found.
        let icon = sni_icon::convert_best(&props.icon_pixmap)
            .map(Icon::Argb)
            .or_else(|| {
                icon_theme::lookup(&props.icon_name, &props.icon_theme_path).map(Icon::Png)
            });
        let item = Item {
            id: self.next_id,
            bus_name: bus_name.to_owned(),
            path: path.to_owned(),
            owner,
            menu_path: props.menu,
        };
        self.next_id += 1;
        self.send(TrayEvent::Added {
            id: item.id,
            title,
            has_menu: item.has_menu(),
            icon,
        });
        self.items.push(item);
    }

    /// Removes the item at (`bus_name`, `path`), if tracked (Host mode: the
    /// Watcher said so, and tells the other hosts itself).
    pub(crate) fn remove(&mut self, bus_name: &str, path: &str) {
        let _gone = self.remove_where(|i| i.bus_name == bus_name && i.path == path);
    }

    /// `name`'s owner disconnected: removes every item registered under it
    /// (one app may register several) or owned by it. Without this a quit
    /// app's icon would stay forever. Returns the removed items' entries.
    pub(crate) fn remove_owner(&mut self, name: &str) -> Vec<String> {
        self.remove_where(|i| i.bus_name == name || i.owner == name)
    }

    /// Removes the matching items and tells the main thread.
    fn remove_where(&mut self, matches: impl Fn(&Item) -> bool) -> Vec<String> {
        let (gone, kept): (Vec<Item>, Vec<Item>) = std::mem::take(&mut self.items)
            .into_iter()
            .partition(&matches);
        self.items = kept;
        for item in &gone {
            self.send(TrayEvent::Removed { id: item.id });
        }
        gone.iter().map(Item::entry).collect()
    }

    /// An item's `NewIcon`: re-reads just its `IconPixmap`.
    pub(crate) async fn new_icon(&mut self, msg: &Message) {
        let header = msg.header();
        let (Some(sender), Some(signal_path)) = (header.sender(), header.path()) else {
            return;
        };
        let Some(item) = self
            .items
            .iter()
            .find(|i| i.owner == sender.as_str() && i.path == signal_path.as_str())
        else {
            return;
        };
        let (id, bus_name, path) = (item.id, item.bus_name.clone(), item.path.clone());
        let reply = self
            .conn
            .call_method(
                Some(bus_name.as_str()),
                path.as_str(),
                Some("org.freedesktop.DBus.Properties"),
                "Get",
                &(ITEM_INTERFACE, "IconPixmap"),
            )
            .await;
        // Read typed, straight from the variant (see `ItemProps` for why).
        let icon = reply.ok().and_then(|r| {
            r.body()
                .deserialize::<as_value::Deserialize<'_, Vec<Pixmap>>>()
                .ok()
                .and_then(|pixmaps| sni_icon::convert_best(&pixmaps.0))
        });
        // A NewIcon without a usable pixmap keeps the old picture, as in C.
        if let Some(argb) = icon {
            self.send(TrayEvent::Icon {
                id,
                icon: Icon::Argb(argb),
            });
        }
    }

    /// Runs a command from the main thread.
    pub(crate) async fn command(&mut self, command: TrayCommand) {
        match command {
            TrayCommand::FetchMenu { request, item } => {
                let entries = match self.items.iter().find(|i| i.id == item) {
                    Some(i) if i.has_menu() => {
                        dbusmenu::fetch(&self.conn, &i.bus_name, &i.menu_path)
                            .await
                            .unwrap_or_else(|err| {
                                eprintln!(
                                    "stickBar: GetLayout failed for {}{} ({err})",
                                    i.bus_name, i.menu_path
                                );
                                Vec::new()
                            })
                    }
                    _ => Vec::new(),
                };
                // Answered even when empty, so the main thread stops waiting.
                self.send(TrayEvent::Menu { request, entries });
            }
            TrayCommand::MenuClick { item, entry } => {
                if let Some(i) = self.items.iter().find(|i| i.id == item)
                    && let Err(err) =
                        dbusmenu::click(&self.conn, &i.bus_name, &i.menu_path, entry).await
                {
                    eprintln!("stickBar: tray: sending a menu click failed ({err})");
                }
            }
            TrayCommand::Activate { item } => {
                if let Some(i) = self.items.iter().find(|i| i.id == item)
                    && let Err(err) = self.activate(&i.bus_name, &i.path).await
                {
                    eprintln!("stickBar: tray: Activate failed ({err})");
                }
            }
        }
    }

    /// `Activate(0, 0)`, without waiting for the reply (the position
    /// arguments are vestigial in real items).
    async fn activate(&self, bus_name: &str, path: &str) -> zbus::Result<()> {
        let msg = Message::method_call(path, "Activate")?
            .destination(bus_name)?
            .interface(ITEM_INTERFACE)?
            .with_flags(Flags::NoReplyExpected)?
            .build(&(0_i32, 0_i32))?;
        self.conn.send(&msg).await
    }

    /// The unique name owning `name`.
    async fn name_owner(&self, name: &str) -> zbus::Result<String> {
        let reply = self
            .conn
            .call_method(
                Some("org.freedesktop.DBus"),
                "/org/freedesktop/DBus",
                Some("org.freedesktop.DBus"),
                "GetNameOwner",
                &name,
            )
            .await?;
        reply.body().deserialize::<String>()
    }

    /// The properties the tray uses, of the item at (`bus_name`, `path`).
    async fn get_all(&self, bus_name: &str, path: &str) -> zbus::Result<ItemProps> {
        let reply = self
            .conn
            .call_method(
                Some(bus_name),
                path,
                Some("org.freedesktop.DBus.Properties"),
                "GetAll",
                &ITEM_INTERFACE,
            )
            .await?;
        reply.body().deserialize()
    }
}

/// The `GetAll` properties the tray uses.
///
/// **Read with a hand-written serde visitor, not as `HashMap<String,
/// OwnedValue>`.** Inside a generic zvariant `Value`, an `ay` is stored as one
/// `Value` per byte, so `CurseForge`'s 256x256 `IconPixmap` (262 KB on the wire)
/// became a vector of several MB, briefly, on every add. Freeing a block that
/// big also raised glibc's adaptive mmap threshold, after which a popup's
/// full-size temporary image stayed resident instead of being unmapped:
/// `test_calendar` measured +6.1 MB instead of +3.8 MB for opening the
/// calendar. So `IconPixmap` is read typed, the text properties normally, and
/// everything else (`ToolTip`, `AttentionIconPixmap`, ... can carry pixmaps
/// too) is skipped without being stored.
#[derive(Debug, Default)]
struct ItemProps {
    /// SNI `Id`.
    id: String,
    /// SNI `Title`.
    title: String,
    /// SNI `Menu` (an object path).
    menu: String,
    /// SNI `IconName`, for the PNG fallback.
    icon_name: String,
    /// SNI `IconThemePath`, searched first by the PNG fallback.
    icon_theme_path: String,
    /// SNI `IconPixmap`.
    icon_pixmap: Vec<Pixmap>,
}

impl Type for ItemProps {
    /// On the wire it's the `a{sv}` dict `GetAll` returns.
    const SIGNATURE: &'static Signature = <HashMap<String, OwnedValue> as Type>::SIGNATURE;
}

impl<'de> de::Deserialize<'de> for ItemProps {
    fn deserialize<D: de::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        deserializer.deserialize_map(PropsVisitor)
    }
}

/// Fills an [`ItemProps`] from the dict, one entry at a time.
struct PropsVisitor;

impl<'de> de::Visitor<'de> for PropsVisitor {
    type Value = ItemProps;

    fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("a StatusNotifierItem property dict")
    }

    fn visit_map<A: de::MapAccess<'de>>(self, mut map: A) -> Result<ItemProps, A::Error> {
        let mut props = ItemProps::default();
        while let Some(key) = map.next_key::<String>()? {
            let field = match key.as_str() {
                "Id" => &mut props.id,
                "Title" => &mut props.title,
                "Menu" => &mut props.menu,
                "IconName" => &mut props.icon_name,
                "IconThemePath" => &mut props.icon_theme_path,
                "IconPixmap" => {
                    props.icon_pixmap = map
                        .next_value::<as_value::Deserialize<'de, Vec<Pixmap>>>()?
                        .0;
                    continue;
                }
                _ => {
                    let _skipped = map.next_value::<de::IgnoredAny>()?;
                    continue;
                }
            };
            // Small: a string or an object path.
            let value = map.next_value::<OwnedValue>()?;
            *field = text_of(&value);
        }
        Ok(props)
    }
}
