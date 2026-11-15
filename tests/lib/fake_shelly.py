"""Fake Shelly Gen3 plugs for test_shelly.py: so no test ever switches a real one.

Each FakePlug is a tiny HTTP server on 127.0.0.1 (a free port), in a thread of
the test process, answering the two Gen2+ RPC calls the bar makes:

  GET /rpc/Switch.GetStatus?id=0   -> {"id":0,"output":bool,"apower":W,...}
  GET /rpc/Switch.Set?id=0&on=B    -> {"was_on":bool}, and switches

It keeps its own on/off state and logs every request path (`requests`), so a
test can see what the bar asked, and when. With `password=True` it answers
every call 401, as a plug with authentication turned on does.

A plug that's unplugged is just a port nobody listens on: `dead_addr()`.
"""

import json
import socket
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse


class FakePlug:
    def __init__(self, on=False, watts=0.0, password=False):
        self.on, self.watts, self.password = on, watts, password
        self.requests = []
        self._lock = threading.Lock()
        plug = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.0"

            def do_GET(self):  # noqa: N802 (http.server's name)
                plug._handle(self)

            def log_message(self, *args):
                pass  # quiet

        self._server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        self._thread.start()

    @property
    def addr(self):
        host, port = self._server.server_address
        return f"{host}:{port}"

    def sets(self):
        """The `on` values of every Switch.Set received, in order."""
        with self._lock:
            return [parse_qs(urlparse(p).query).get("on", ["?"])[0]
                    for p in self.requests if p.startswith("/rpc/Switch.Set")]

    def count(self):
        with self._lock:
            return len(self.requests)

    def _handle(self, handler):
        with self._lock:
            self.requests.append(handler.path)
            url = urlparse(handler.path)
            query = parse_qs(url.query)
            if self.password:
                status, body = 401, {"code": 401, "message": "unauthorized"}
            elif url.path == "/rpc/Switch.GetStatus":
                status, body = 200, {
                    "id": 0, "source": "init", "output": self.on,
                    "apower": self.watts if self.on else 0.0,
                    "voltage": 231.4, "current": 0.2, "temperature": {"tC": 38.1},
                }
            elif url.path == "/rpc/Switch.Set":
                was_on = self.on
                self.on = query.get("on", ["false"])[0] == "true"
                status, body = 200, {"was_on": was_on}
            else:
                status, body = 404, {"code": 404, "message": "No handler"}
        data = json.dumps(body).encode()
        handler.send_response(status)
        handler.send_header("Content-Type", "application/json")
        handler.send_header("Content-Length", str(len(data)))
        handler.end_headers()
        handler.wfile.write(data)

    def close(self):
        self._server.shutdown()
        self._server.server_close()


def dead_addr():
    """A localhost port nothing listens on (connect is refused at once)."""
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return f"127.0.0.1:{s.getsockname()[1]}"
