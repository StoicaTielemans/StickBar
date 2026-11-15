#!/usr/bin/env python3
"""Stands in for curl in test_proxmox.py, so the test never touches a real
Proxmox server or the network. The bar runs it through
STICKBAR_TEST_PROXMOX_CURL with the same arguments it gives curl, and the API
token as a curl config line on stdin (`-K -`).

What it answers comes from the JSON file at $FAKE_PROXMOX_STATE:
{"answer": "up" | "off" | "401", "shutdown": "ok" | "403"}
  "up"  -> a cluster/resources body (1 node, 3 guests + a template, 2
           storages), then a newline and 200, exit 0 (what --write-out adds)
  "off" -> exit 7, curl's "couldn't connect"
  "401" -> an error body, a newline and 401, exit 0
A POST (`--request POST`, the shutdown) answers per "shutdown" instead
(default "ok"): a task id and 200, or 403 as for a token without
Sys.PowerMgmt. "off" still means curl couldn't connect.

Every run appends one JSON line to $FAKE_PROXMOX_LOG: its argv, what it read
on stdin, and its SigBlk from /proc/self/status, so the test can check the
token never shows in argv and the bar reset its signal mask.
"""

import json
import os
import sys

BODY = {"data": [
    {"type": "node", "node": "pve", "status": "online", "cpu": 0.12, "maxcpu": 8,
     "mem": 8589934592, "maxmem": 34359738368, "disk": 1.0e10, "maxdisk": 1.0e11,
     "uptime": 273600},
    {"type": "lxc", "vmid": 100, "name": "pihole", "status": "running", "cpu": 0.01,
     "maxcpu": 1, "mem": 134217728, "maxmem": 536870912, "uptime": 3600},
    {"type": "qemu", "vmid": 101, "name": "homeassistant", "status": "running",
     "cpu": 0.05, "maxcpu": 2, "mem": 2147483648, "maxmem": 4294967296, "uptime": 3600},
    {"type": "qemu", "vmid": 102, "name": "windows", "status": "stopped", "cpu": 0,
     "maxcpu": 4, "mem": 0, "maxmem": 8589934592, "uptime": 0},
    {"type": "qemu", "vmid": 9000, "name": "debian-template", "status": "stopped",
     "template": 1},
    {"type": "storage", "storage": "local", "status": "available", "disk": 1.0e10,
     "maxdisk": 1.0e11},
    {"type": "storage", "storage": "local-lvm", "status": "available", "disk": 5.0e10,
     "maxdisk": 4.0e11},
]}

if __name__ == "__main__":
    stdin = sys.stdin.read()
    sigblk = next(l for l in open("/proc/self/status") if l.startswith("SigBlk:")).split()[1]
    with open(os.environ["FAKE_PROXMOX_LOG"], "a") as log:
        log.write(json.dumps({"argv": sys.argv, "stdin": stdin, "sigblk": sigblk}) + "\n")
    with open(os.environ["FAKE_PROXMOX_STATE"]) as fh:
        state = json.load(fh)
    answer = state["answer"]
    if answer == "off":
        sys.exit(7)
    if "--request" in sys.argv and "POST" in sys.argv:
        if state.get("shutdown", "ok") == "403":
            sys.stdout.write('{"data":null}\n403')
        else:
            sys.stdout.write('{"data":"UPID:pve:0001:shutdown"}\n200')
        sys.exit(0)
    if answer == "401":
        sys.stdout.write('{"data":null}\n401')
        sys.exit(0)
    sys.stdout.write(json.dumps(BODY) + "\n200")
