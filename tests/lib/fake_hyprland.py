"""A stand-in for Hyprland's two IPC sockets, so tests can drive the workspace
pills -- and click them -- without ever touching the user's real workspaces.

Hyprland serves both sockets from $XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/:

  .socket.sock   request socket: one connection per command. The client writes
                 the command, shuts down its write side, and reads the reply to
                 EOF. `j/workspaces` and `j/activeworkspace` reply in JSON.
  .socket2.sock  event socket: a persistent connection Hyprland pushes
                 `name>>payload` lines down.

FakeHyprland serves both from its own instance directory (pass `signature` as
the bar's HYPRLAND_INSTANCE_SIGNATURE), answers the two JSON queries from
`workspaces` / `active` (which the test changes as it goes), records every
command it receives, and pushes events on request. Standard library only.
"""

import json
import os
import shutil
import socket
import threading
import time


class FakeHyprland:
    def __init__(self):
        runtime = os.environ["XDG_RUNTIME_DIR"]
        self.signature = f"stickbar-test-{os.getpid()}"
        # Short on purpose: a Unix socket path has to fit in 108 bytes.
        self.dir = os.path.join(runtime, "hypr", self.signature)
        os.makedirs(self.dir, exist_ok=True)
        self.workspaces = {}       # id -> window count
        self.active = None         # focused workspace id
        self.reject_lua = False    # reply "error" to `hl.dsp...` dispatches
        self.commands = []         # every request-socket command, in order
        self.event_connections = 0
        self._events = None        # the latest event connection
        self._lock = threading.Lock()
        self._stop = False
        self._requests = self._listen(".socket.sock")
        self._event_listener = self._listen(".socket2.sock")
        threading.Thread(target=self._serve_requests, daemon=True).start()
        threading.Thread(target=self._serve_events, daemon=True).start()

    def _listen(self, name):
        path = os.path.join(self.dir, name)
        if os.path.exists(path):
            os.unlink(path)
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.bind(path)
        sock.listen(16)
        sock.settimeout(0.2)
        return sock

    # ---- request socket ------------------------------------------------------

    def _reply_for(self, command):
        if command == "j/workspaces":
            return json.dumps([{"id": i, "windows": n, "name": str(i)} for i, n in self.workspaces.items()])
        if command == "j/activeworkspace":
            return json.dumps({"id": self.active if self.active is not None else -1})
        if command.startswith("dispatch "):
            if self.reject_lua and "hl.dsp" in command:
                return "error: expected a dispatcher"
            return "ok"
        return "unknown request"

    def _serve_requests(self):
        while not self._stop:
            try:
                conn, _ = self._requests.accept()
            except (TimeoutError, OSError):
                continue
            with conn:
                conn.settimeout(2.0)
                data = b""
                try:
                    while chunk := conn.recv(4096):  # until the client's shutdown(SHUT_WR)
                        data += chunk
                except OSError:
                    continue
                command = data.decode(errors="replace")
                with self._lock:
                    self.commands.append(command)
                    reply = self._reply_for(command)
                try:
                    conn.sendall(reply.encode())
                except OSError:
                    pass

    # ---- event socket --------------------------------------------------------

    def _serve_events(self):
        while not self._stop:
            try:
                conn, _ = self._event_listener.accept()
            except (TimeoutError, OSError):
                continue
            with self._lock:
                self.event_connections += 1
                if self._events:
                    self._events.close()
                self._events = conn

    def send_event(self, line):
        """Pushes one `name>>payload` line to the bar."""
        with self._lock:
            if self._events:
                self._events.sendall((line + "\n").encode())

    def close_events(self):
        """Hangs up the event connection, like a Hyprland restart would."""
        with self._lock:
            if self._events:
                self._events.close()
                self._events = None

    # ---- helpers for tests ---------------------------------------------------

    def commands_matching(self, prefix):
        with self._lock:
            return [c for c in self.commands if c.startswith(prefix)]

    def wait_for(self, predicate, timeout=3.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if predicate():
                return True
            time.sleep(0.05)
        return False

    def close(self):
        self._stop = True
        self.close_events()
        for sock in (self._requests, self._event_listener):
            sock.close()
        shutil.rmtree(self.dir, ignore_errors=True)
