#!/usr/bin/env python3
"""Fake tray app for tests/test_tray.py (run as a subprocess).

    python3 fake_tray.py LOGFILE LIFETIME_SECONDS

Exports a StatusNotifierItem (Title "StickbarTestTray", a small grey
IconPixmap) with a com.canonical.dbusmenu menu at /MenuBar, registers it with
whichever org.kde.StatusNotifierWatcher is running (usually the user's own
bar, so its icon briefly shows there too), writes "registered" and then one
"event <id> <label>" line per menu click it receives to LOGFILE, and exits
after LIFETIME_SECONDS. The menu covers what the tray dropdown draws:
access-key underscores, separators (cards), a checkmark, a radio pair, a
disabled entry and a submenu marker. Needs PyGObject (Gio).
"""
import os
import sys

from gi.repository import Gio, GLib

LOG = open(sys.argv[1], "a", buffering=1)
LIFETIME = int(sys.argv[2]) if len(sys.argv) > 2 else 30
ITEM_PATH, MENU_PATH = "/StatusNotifierItem", "/MenuBar"
NAME = f"org.kde.StatusNotifierItem-{os.getpid()}-1"

# (id, properties) in menu order; id 0 is the root.
ENTRIES = [
    (1, {"label": "_Open Window"}),
    (2, {"type": "separator"}),
    (3, {"label": "Show _Notifications", "toggle-type": "checkmark", "toggle-state": 1}),
    (4, {"label": "Mode A", "toggle-type": "radio", "toggle-state": 1}),
    (5, {"label": "Mode B", "toggle-type": "radio", "toggle-state": 0}),
    (6, {"type": "separator"}),
    (7, {"label": "Disabled Item", "enabled": False}),
    (8, {"label": "More", "children-display": "submenu"}),
    (9, {"type": "separator"}),
    (10, {"label": "_Quit"}),
]
LABELS = {i: p.get("label", "") for i, p in ENTRIES}

XML = """
<node>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Category" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <method name="Activate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
  </interface>
  <interface name="com.canonical.dbusmenu">
    <property name="Version" type="u" access="read"/>
    <property name="Status" type="s" access="read"/>
    <method name="GetLayout">
      <arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="as" direction="in"/>
      <arg type="u" direction="out"/><arg type="(ia{sv}av)" direction="out"/>
    </method>
    <method name="Event">
      <arg type="i" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="in"/>
      <arg type="u" direction="in"/>
    </method>
    <method name="AboutToShow"><arg type="i" direction="in"/><arg type="b" direction="out"/></method>
  </interface>
</node>
"""


def variant_props(props):
    out = {}
    for key, value in props.items():
        if isinstance(value, bool):
            out[key] = GLib.Variant("b", value)
        elif isinstance(value, int):
            out[key] = GLib.Variant("i", value)
        else:
            out[key] = GLib.Variant("s", value)
    return out


def layout():
    children = [GLib.Variant("(ia{sv}av)", (i, variant_props(p), [])) for i, p in ENTRIES]
    return (0, {"children-display": GLib.Variant("s", "submenu")}, children)


def icon():
    size = 16
    pixel = bytes([255, 0x9a, 0x9a, 0x9a])  # ARGB, network byte order
    return [(size, size, pixel * size * size)]


def get_prop(conn, sender, path, iface, prop):
    if iface == "org.kde.StatusNotifierItem":
        return {
            "Id": GLib.Variant("s", "stickbar_test_tray"),
            "Title": GLib.Variant("s", "StickbarTestTray"),
            "Category": GLib.Variant("s", "ApplicationStatus"),
            "Status": GLib.Variant("s", "Active"),
            "Menu": GLib.Variant("o", MENU_PATH),
            "ItemIsMenu": GLib.Variant("b", False),
            "IconName": GLib.Variant("s", ""),
            "IconPixmap": GLib.Variant("a(iiay)", icon()),
        }[prop]
    return {"Version": GLib.Variant("u", 3), "Status": GLib.Variant("s", "normal")}[prop]


def on_call(conn, sender, path, iface, method, params, inv):
    if method == "GetLayout":
        inv.return_value(GLib.Variant("(u(ia{sv}av))", (1, layout())))
    elif method == "Event":
        entry_id, event = params[0], params[1]
        if event == "clicked":
            LOG.write(f"event {entry_id} {LABELS.get(entry_id, '?')}\n")
        inv.return_value(None)
    elif method == "AboutToShow":
        inv.return_value(GLib.Variant("(b)", (False,)))
    else:
        LOG.write(f"{method}\n")
        inv.return_value(None)


def on_bus(conn, name):
    info = Gio.DBusNodeInfo.new_for_xml(XML)
    conn.register_object(ITEM_PATH, info.interfaces[0], on_call, get_prop, None)
    conn.register_object(MENU_PATH, info.interfaces[1], on_call, get_prop, None)


def on_registered(conn, result):
    conn.call_finish(result)
    LOG.write("registered\n")


def on_name(conn, name):
    # Asynchronous on purpose: the watcher fetches this item's properties
    # while handling the call, so a blocking call here would deadlock until
    # the D-Bus timeout (seen while writing this).
    conn.call("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
              "RegisterStatusNotifierItem", GLib.Variant("(s)", (NAME,)), None,
              Gio.DBusCallFlags.NONE, 5000, None, on_registered)


loop = GLib.MainLoop()
Gio.bus_own_name(Gio.BusType.SESSION, NAME, Gio.BusNameOwnerFlags.NONE, on_bus, on_name, None)
GLib.timeout_add_seconds(LIFETIME, loop.quit)
loop.run()
