#!/usr/bin/env python3
"""The Proxmox box and dropdown follow the server, and Wake-on-LAN sends the
right packet, without a real server, curl or a LAN broadcast.

The bar gets a settings file in a temp dir (STICKBAR_TEST_PROXMOX_CONF), the
fake curl tests/lib/fake_proxmox.py (STICKBAR_TEST_PROXMOX_CURL), and a UDP
socket on localhost to send the magic packet to (STICKBAR_TEST_PROXMOX_WOL_ADDR).
A test-mode bar never polls by itself, so each poll is the test's
`refresh proxmox`:

  - a bar without the settings variable has no Proxmox box (test bars never
    read the user's real file);
  - the box shows `--` before the first poll;
  - "up": the box reads `2/3` (the template left out), `dump proxmox` lists
    the guests, and the right-click dropdown shows 3 guest rows and no Wake chip;
  - "off": the box reads `off`, and the open dropdown re-lays out with a Wake chip
    that shows the hand cursor;
  - a click on Wake: exactly the 102-byte magic packet arrives for the
    configured MAC, and the box turns to "waking";
  - "up" again ends waking, and the dropdown's chip becomes Shut down;
  - Shut down takes two clicks: the first only arms it (nothing sent), a
    second after the 5 s window re-arms instead of sending, a quick second
    one POSTs `command=shutdown` to /nodes/pve/status;
  - a 403 answer (token without Sys.PowerMgmt) shows the error and doesn't
    start shutting down; a 200 does (the count turns peach), "up" polls keep it, and an
    "off" poll ends it with the Wake chip back;
  - a 401 keeps the guest list and reports the error;
  - every curl run: the token only on stdin (never in argv), the pin passed
    as --pinnedpubkey with --insecure, and an empty signal mask (SigBlk 0).
"""

import json
import socket
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import common  # noqa: E402

FAKE = Path(__file__).resolve().parent / "lib" / "fake_proxmox.py"
TOKEN = "root@pam!stickbar=SECRET-0b3c-11aa"
PIN = "sha256//dGVzdHBpbg=="
MAC = bytes([0x7c, 0x05, 0x07, 0xb0, 0xc4, 0xc0])


def main():
    common.require_session()
    binary = common.bar_binary()
    common.require_binary(binary)
    failures = []

    # Without the settings variable a test bar has no Proxmox module at all.
    with common.ControlledBar(binary) as bar:
        bar.wait_ready()
        if bar.must("dump proxmox")["enabled"]:
            failures.append("a test bar without STICKBAR_TEST_PROXMOX_CONF read settings")

    with tempfile.TemporaryDirectory() as tmp:
        conf = Path(tmp) / "proxmox.conf"
        conf.write_text(f"url = https://127.0.0.1:1\ntoken = {TOKEN}\npin = {PIN}\n"
                        f"mac = {MAC.hex(':')}\n")
        conf.chmod(0o600)
        state = Path(tmp) / "state.json"
        log = Path(tmp) / "runs.log"
        log.write_text("")
        wol = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        wol.bind(("127.0.0.1", 0))
        wol.settimeout(3)
        env = {"STICKBAR_TEST_PROXMOX_CONF": str(conf),
               "STICKBAR_TEST_PROXMOX_CURL": str(FAKE),
               "STICKBAR_TEST_PROXMOX_WOL_ADDR": f"127.0.0.1:{wol.getsockname()[1]}",
               "FAKE_PROXMOX_STATE": str(state), "FAKE_PROXMOX_LOG": str(log)}

        def answer(what, shutdown="ok"):
            state.write_text(json.dumps({"answer": what, "shutdown": shutdown}))

        def posts():
            return [r for r in log.read_text().splitlines() if '"POST"' in r]

        with common.ControlledBar(binary, env_extra=env) as bar:
            def poll():
                bar.must("refresh proxmox")
                deadline = time.time() + 10
                while time.time() < deadline:
                    d = bar.must("dump proxmox")
                    if not d["polling"]:
                        time.sleep(0.2)  # the redraw after the result
                        return d
                    time.sleep(0.05)
                common.fail("the Proxmox poll never finished")

            bar.wait_ready(("proxmox",))
            d = bar.must("dump proxmox")
            if (d["phase"], d["label"]) != ("unknown", "--"):
                failures.append(f"before a poll: {d['phase']} {d['label']!r}")

            answer("up")
            d = poll()
            guests = ["100 pihole running", "101 homeassistant running", "102 windows stopped"]
            if (d["phase"], d["label"], d["guests"], d["nodes"], d["storages"]) != (
                    "up", "2/3", guests, 1, 2):
                failures.append(f"after 'up': {d}")
            bar.click_module("proxmox")
            up_menu = bar.wait_menu(True)
            kind = up_menu.get("proxmox_chip", {}).get("kind")
            if up_menu["proxmox_rows"] != 3 or kind != "shutdown":
                failures.append(f"'up' dropdown: {up_menu['proxmox_rows']} rows, chip {kind}")

            answer("off")
            d = poll()
            if (d["phase"], d["label"], d["guests"]) != ("off", "off", []):
                failures.append(f"after 'off': {d}")
            off_menu = bar.wait_menu(True)
            chip = off_menu.get("proxmox_chip")
            if chip is None or chip["kind"] != "wake":
                common.fail("no Wake chip in the dropdown while the server is off")
            if off_menu["height"] == up_menu["height"]:
                failures.append("the dropdown didn't re-lay out for 'off'")
            shape = bar.must(f"menu-cursor {chip['x']:.1f} {chip['y']:.1f}")["shape"]
            if shape != "pointer":
                failures.append(f"cursor over the Wake chip: {shape}")

            bar.must(f"menu-click {chip['x']:.1f} {chip['y']:.1f} left")
            try:
                packet, _addr = wol.recvfrom(1024)
            except socket.timeout:
                packet = b""
            if packet != b"\xff" * 6 + MAC * 16:
                failures.append(f"magic packet: {len(packet)} bytes {packet[:12].hex()}...")
            d = bar.must("dump proxmox")
            if (d["phase"], d["label"]) != ("waking", "…"):
                failures.append(f"after Wake: {d['phase']} {d['label']!r}")
            # The wake's own poll (still "off") must leave it waking.
            d = poll()
            if d["phase"] != "waking":
                failures.append(f"an 'off' poll while waking ended it: {d['phase']}")

            answer("up")
            d = poll()
            if (d["phase"], d["label"]) != ("up", "2/3"):
                failures.append(f"'up' after waking: {d['phase']} {d['label']!r}")

            # Shut down: two clicks, a 403 first.
            answer("up", shutdown="403")
            menu = bar.wait_menu(True)
            chip = menu.get("proxmox_chip") or {}
            if chip.get("kind") != "shutdown":
                common.fail(f"no Shut down chip while up: {menu}")
            click = f"menu-click {chip['x']:.1f} {chip['y']:.1f} left"
            bar.must(click)
            if not bar.must("menu").get("proxmox_chip", {}).get("armed") or posts():
                failures.append("the first Shut down click didn't just arm it")
            time.sleep(5.3)  # past the arm window: this click re-arms
            bar.must(click)
            time.sleep(0.3)
            if not bar.must("menu").get("proxmox_chip", {}).get("armed") or posts():
                failures.append("a second click after the 5 s window sent the shutdown")
            bar.must(click)
            deadline = time.time() + 5
            while time.time() < deadline and not bar.must("dump proxmox")["action_error"]:
                time.sleep(0.05)
            d = bar.must("dump proxmox")
            if len(posts()) != 1 or "Sys.PowerMgmt" not in d["action_error"] or d["phase"] != "up":
                failures.append(f"a 403 shutdown: {len(posts())} POSTs, {d}")

            # The error line pushed the chip down: click where it is now.
            answer("up")
            chip = bar.must("menu")["proxmox_chip"]
            click = f"menu-click {chip['x']:.1f} {chip['y']:.1f} left"
            bar.must(click)
            bar.must(click)
            deadline = time.time() + 5
            while time.time() < deadline and bar.must("dump proxmox")["phase"] != "shutting_down":
                time.sleep(0.05)
            d = poll()  # an "up" poll while stopping guests keeps it
            if (d["phase"], d["label"], d["action_error"]) != (
                    "shutting_down", "2/3", ""):
                failures.append(f"after a sent shutdown: {d}")
            if "proxmox_chip" in bar.wait_menu(True):
                failures.append("a chip while shutting down")
            post = json.loads(posts()[-1])["argv"]
            if not (post[-1].endswith("/api2/json/nodes/pve/status")
                    and "command=shutdown" in post):
                failures.append(f"the shutdown request: {post}")
            answer("off")
            d = poll()
            menu = bar.wait_menu(True)
            if d["phase"] != "off" or menu.get("proxmox_chip", {}).get("kind") != "wake":
                failures.append(f"'off' after shutting down: {d['phase']}, {menu.get('proxmox_chip')}")

            answer("up")
            poll()
            answer("401")
            d = poll()
            if d["phase"] != "error" or "401" not in d["last_error"] or len(d["guests"]) != 3:
                failures.append(f"after a 401: {d}")
            if bar.must("dump proxmox")["label"] != "!":
                failures.append("the box doesn't show the error")
        wol.close()

        runs = [json.loads(line) for line in log.read_text().splitlines() if line.strip()]
        if len(runs) < 10:
            failures.append(f"only {len(runs)} curl runs logged")
        for run in runs:
            argv = run["argv"]
            if any(TOKEN in a or "SECRET" in a for a in argv):
                failures.append(f"the token is in curl's argv: {argv}")
                break
            if f"Authorization: PVEAPIToken={TOKEN}" not in run["stdin"]:
                failures.append(f"no Authorization header on stdin: {run['stdin']!r}")
                break
            want = ("/api2/json/nodes/pve/status" if "POST" in argv
                    else "/api2/json/cluster/resources")
            if "--insecure" not in argv or PIN not in argv or not argv[-1].endswith(want):
                failures.append(f"curl's arguments: {argv}")
                break
        masks = {run["sigblk"] for run in runs}
        if masks != {"0000000000000000"}:
            failures.append(f"curl ran with blocked signals: SigBlk {masks}")

    if failures:
        common.fail("; ".join(failures))
    common.ok("no box without settings; '--' then 2/3 with 3 guest rows; 'off' grew a Wake "
              "chip with a hand cursor; Wake sent the exact 102-byte packet and showed "
              "waking until 'up'; Shut down armed on one click, re-armed after 5 s, a 403 "
              "showed the error, a 200 shut down until 'off'; a 401 kept the guests; the "
              "token stayed out of argv and every curl ran with SigBlk 0")


if __name__ == "__main__":
    main()
