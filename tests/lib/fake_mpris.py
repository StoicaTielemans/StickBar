#!/usr/bin/env python3
"""Fake MPRIS player for tests/test_media.py (run as a subprocess).

    python3 fake_mpris.py LOGFILE LIFETIME_SECONDS [NAME]

Owns org.mpris.MediaPlayer2.stickbartest (Identity "StickbarTestPlayer"), or
with NAME, org.mpris.MediaPlayer2.stickbartest_NAME (Identity
"StickbarTestPlayer-NAME") -- several can run at once. Plays no audio. It
starts Paused with a fixed track, writes "owned" and then every method call
it receives to LOGFILE (one per line), toggles PlaybackStatus on PlayPause
(emitting PropertiesChanged, as a real player does), and exits after
LIFETIME_SECONDS. Lets the test click the dropdown's controls without
touching whatever real player the user has open. Needs PyGObject (Gio).
"""
import sys
from gi.repository import Gio, GLib

LOG = open(sys.argv[1], "a", buffering=1)
LIFETIME = int(sys.argv[2]) if len(sys.argv) > 2 else 60
PATH = "/org/mpris/MediaPlayer2"
NAME = sys.argv[3] if len(sys.argv) > 3 else ""
BUS_NAME = "org.mpris.MediaPlayer2.stickbartest" + (f"_{NAME}" if NAME else "")
IDENTITY = "StickbarTestPlayer" + (f"-{NAME}" if NAME else "")

XML = """
<node>
  <interface name="org.mpris.MediaPlayer2">
    <property name="Identity" type="s" access="read"/>
  </interface>
  <interface name="org.mpris.MediaPlayer2.Player">
    <method name="PlayPause"/><method name="Next"/><method name="Previous"/>
    <method name="Play"/><method name="Pause"/>
    <property name="PlaybackStatus" type="s" access="read"/>
    <property name="Metadata" type="a{sv}" access="read"/>
    <property name="Position" type="x" access="read"/>
    <property name="CanGoNext" type="b" access="read"/>
    <property name="CanGoPrevious" type="b" access="read"/>
  </interface>
</node>
"""

state = {"status": "Paused"}
conn_ref = {}


def props(iface):
    if iface == "org.mpris.MediaPlayer2":
        return {"Identity": GLib.Variant("s", IDENTITY)}
    return {
        "PlaybackStatus": GLib.Variant("s", state["status"]),
        "Metadata": GLib.Variant("a{sv}", {
            "xesam:title": GLib.Variant("s", "Fake Track Title"),
            "xesam:artist": GLib.Variant("as", ["Fake Artist"]),
            "xesam:album": GLib.Variant("s", "Fake Album"),
            "mpris:length": GLib.Variant("x", 200_000_000),
        }),
        "Position": GLib.Variant("x", 30_000_000),
        "CanGoNext": GLib.Variant("b", True),
        "CanGoPrevious": GLib.Variant("b", True),
    }


def on_call(conn, sender, path, iface, method, params, inv):
    LOG.write(f"{method}\n")
    if method == "PlayPause":
        state["status"] = "Playing" if state["status"] != "Playing" else "Paused"
        conn.emit_signal(None, PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         GLib.Variant("(sa{sv}as)", ("org.mpris.MediaPlayer2.Player",
                                                     {"PlaybackStatus": GLib.Variant("s", state["status"])}, [])))
    inv.return_value(None)


def on_get(conn, sender, path, iface, prop):
    return props(iface)[prop]


def on_bus(conn, name):
    info = Gio.DBusNodeInfo.new_for_xml(XML)
    for iface in info.interfaces:
        conn.register_object(PATH, iface, on_call, on_get, None)


def on_name(conn, name):
    LOG.write("owned\n")


loop = GLib.MainLoop()
Gio.bus_own_name(Gio.BusType.SESSION, BUS_NAME, Gio.BusNameOwnerFlags.NONE, on_bus, on_name, None)
GLib.timeout_add_seconds(LIFETIME, loop.quit)
loop.run()
