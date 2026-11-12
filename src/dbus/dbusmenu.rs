//! A tray item's right-click menu, over `com.canonical.dbusmenu` (the C bar's
//! `dbus/dbusmenu.c`). Verified against real items while the C bar was written
//! (`docs/modules/tray.md`): `GetLayout(0, 1, [])` gives the flat top-level
//! list the reference bar shows, entries without a `type` are plain items,
//! and `enabled` **defaults to true** when absent (most entries omit it).

use std::{collections::HashMap, time::SystemTime};

use super::unwrap_variant;
use zbus::{
    Connection,
    message::{Flags, Message},
    zvariant::{OwnedValue, Structure, Value},
};

/// The menu interface.
const MENU_INTERFACE: &str = "com.canonical.dbusmenu";

/// `GetLayout`'s reply: a revision, then the root as (id, properties,
/// children), each child a variant holding the same shape.
type Layout = (u32, (i32, HashMap<String, OwnedValue>, Vec<OwnedValue>));

/// A checkbox or radio entry's kind.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Toggle {
    /// `toggle-type: checkmark`.
    Checkmark,
    /// `toggle-type: radio`.
    Radio,
}

/// A checkbox or radio entry: its kind, and whether it's on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ToggleState {
    /// Checkbox or radio.
    pub(crate) kind: Toggle,
    /// `toggle-state == 1` (0 = off, -1 = indeterminate, shown as off).
    pub(crate) on: bool,
}

/// One top-level menu entry.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct MenuEntry {
    /// The id its click event is sent with.
    pub(crate) id: i32,
    /// The label, access-key underscores removed ("" for separators).
    pub(crate) label: String,
    /// `type: separator`: the gap between two cards, not a row.
    pub(crate) separator: bool,
    /// Clickable (true unless the entry says otherwise).
    pub(crate) enabled: bool,
    /// A checkbox or radio entry, if it is one.
    pub(crate) toggle: Option<ToggleState>,
    /// `children-display: submenu`: marked with a chevron, not openable.
    pub(crate) submenu: bool,
}

/// Fetches the top-level entries of the menu at (`bus_name`, `menu_path`).
/// An error leaves the caller with no menu, as in C.
pub(crate) async fn fetch(
    conn: &Connection,
    bus_name: &str,
    menu_path: &str,
) -> zbus::Result<Vec<MenuEntry>> {
    // Some implementations fill a menu in only once AboutToShow(root) has been
    // called. Its reply ("needs update") isn't needed: GetLayout follows anyway.
    if let Err(err) = conn
        .call_method(
            Some(bus_name),
            menu_path,
            Some(MENU_INTERFACE),
            "AboutToShow",
            &0_i32,
        )
        .await
    {
        // Not fatal: some menus don't implement it.
        eprintln!("stickBar: tray: AboutToShow failed for {bus_name}{menu_path} ({err})");
    }
    // parentId 0 (the root), recursionDepth 1 (its direct children only),
    // propertyNames [] (all of them).
    let reply = conn
        .call_method(
            Some(bus_name),
            menu_path,
            Some(MENU_INTERFACE),
            "GetLayout",
            &(0_i32, 1_i32, Vec::<String>::new()),
        )
        .await?;
    let (_revision, (_root_id, _root_props, children)): Layout = reply.body().deserialize()?;
    Ok(children.iter().filter_map(|c| parse_entry(c)).collect())
}

/// Sends `Event(id, "clicked", "", now)`. The method is marked `NoReply` in
/// the real interface, so nothing waits for an answer.
pub(crate) async fn click(
    conn: &Connection,
    bus_name: &str,
    menu_path: &str,
    entry_id: i32,
) -> zbus::Result<()> {
    let now = SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .map_or(0, |d| u32::try_from(d.as_secs()).unwrap_or(u32::MAX));
    // `data` as an empty-string variant: what real hosts send for a click.
    let body = (entry_id, "clicked", Value::from(""), now);
    let msg = Message::method_call(menu_path, "Event")?
        .destination(bus_name)?
        .interface(MENU_INTERFACE)?
        .with_flags(Flags::NoReplyExpected)?
        .build(&body)?;
    conn.send(&msg).await
}

/// One child: a `(ia{sv}av)` structure, possibly inside a variant.
fn parse_entry(child: &Value<'_>) -> Option<MenuEntry> {
    let Value::Structure(structure) = unwrap_variant(child) else {
        return None;
    };
    parse_structure(structure)
}

/// The fields of one `(ia{sv}av)` entry. Its own children (`av`) are always
/// empty at recursion depth 1 and are ignored.
fn parse_structure(structure: &Structure<'_>) -> Option<MenuEntry> {
    let [Value::I32(id), Value::Dict(props), ..] = structure.fields() else {
        return None;
    };
    let mut entry = MenuEntry {
        id: *id,
        label: String::new(),
        separator: false,
        enabled: true,
        toggle: None,
        submenu: false,
    };
    // Gathered first: the two toggle properties can come in either order.
    let (mut kind, mut on) = (None, false);
    for (key, value) in props.iter() {
        let (Value::Str(key), value) = (key, unwrap_variant(value)) else {
            continue;
        };
        // Every other property (icon-name, shortcut, visible, ...) isn't drawn.
        match (key.as_str(), value) {
            ("type", Value::Str(s)) => entry.separator = s.as_str() == "separator",
            ("label", Value::Str(s)) => entry.label = strip_access_keys(s.as_str()),
            ("toggle-type", Value::Str(s)) => {
                kind = match s.as_str() {
                    "checkmark" => Some(Toggle::Checkmark),
                    "radio" => Some(Toggle::Radio),
                    _ => None,
                };
            }
            ("children-display", Value::Str(s)) => entry.submenu = s.as_str() == "submenu",
            ("enabled", Value::Bool(b)) => entry.enabled = *b,
            ("toggle-state", Value::I32(state)) => on = *state == 1,
            _ => {}
        }
    }
    entry.toggle = kind.map(|kind| ToggleState { kind, on });
    Some(entry)
}

/// Drops the access-key markers: `_Quit` -> `Quit`, `__` -> `_` (the GTK
/// mnemonic convention). The bar has no keyboard navigation to use them.
pub(crate) fn strip_access_keys(label: &str) -> String {
    let mut out = String::with_capacity(label.len());
    let mut chars = label.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '_' {
            if chars.peek() == Some(&'_') {
                out.push('_');
                let _second = chars.next();
            }
            continue;
        }
        out.push(c);
    }
    out
}

#[cfg(test)]
mod tests {
    use std::collections::HashMap;

    use zbus::zvariant::{StructureBuilder, Value};

    use super::{MenuEntry, Toggle, ToggleState, parse_entry, strip_access_keys};

    #[test]
    fn access_keys() {
        assert_eq!(strip_access_keys("_Quit"), "Quit");
        assert_eq!(
            strip_access_keys("Show _Notifications"),
            "Show Notifications"
        );
        assert_eq!(strip_access_keys("a__b"), "a_b");
        assert_eq!(strip_access_keys("trailing_"), "trailing");
    }

    /// A child as `GetLayout` sends it: a variant holding (id, props, []).
    fn child(id: i32, props: &[(&str, Value<'static>)]) -> Value<'static> {
        let dict: HashMap<String, Value<'static>> = props
            .iter()
            .map(|(k, v)| {
                (
                    (*k).to_owned(),
                    Value::Value(Box::new(v.try_clone().unwrap())),
                )
            })
            .collect();
        let structure = StructureBuilder::new()
            .add_field(id)
            .add_field(dict)
            .add_field(Vec::<Value<'static>>::new())
            .build()
            .unwrap();
        Value::Value(Box::new(Value::Structure(structure)))
    }

    #[test]
    fn parses_entries() {
        let entry = parse_entry(&child(
            3,
            &[
                ("label", Value::from("Show _Notifications")),
                ("toggle-type", Value::from("checkmark")),
                ("toggle-state", Value::from(1_i32)),
            ],
        ))
        .unwrap();
        assert_eq!(
            entry,
            MenuEntry {
                id: 3,
                label: "Show Notifications".to_owned(),
                separator: false,
                enabled: true,
                toggle: Some(ToggleState {
                    kind: Toggle::Checkmark,
                    on: true,
                }),
                submenu: false,
            }
        );
        let sep = parse_entry(&child(2, &[("type", Value::from("separator"))])).unwrap();
        assert!(sep.separator && sep.label.is_empty());
        let off = parse_entry(&child(
            7,
            &[
                ("enabled", Value::from(false)),
                ("children-display", Value::from("submenu")),
            ],
        ))
        .unwrap();
        assert!(!off.enabled && off.submenu);
    }
}
