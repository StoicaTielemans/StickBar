"""Minimal Wayland socket proxy: listens on a Unix socket and forwards every
connection to the real compositor socket, byte for byte in both directions.

Exists so test_wayland_disconnect.py can simulate the compositor dying (by
killing this proxy) without killing the user's actual Hyprland session -- no
nested compositor (weston/cage/sway) was installed to do it the obvious way.

The one non-obvious part: Wayland passes file descriptors over the socket
(SCM_RIGHTS -- e.g. every wl_shm pool's memfd), so plain byte copying isn't
enough. socket.recv_fds()/send_fds() carry them across; without that the bar
fails as soon as it creates its first shm buffer.

Usage: python3 wlproxy.py LISTEN_PATH TARGET_PATH
"""

import os
import socket
import sys
import threading

MAX_FDS_PER_MSG = 28  # libwayland's own per-message fd limit


def pump(src, dst):
    try:
        while True:
            data, fds, _flags, _addr = socket.recv_fds(src, 65536, MAX_FDS_PER_MSG)
            if not data:
                break
            if fds:
                socket.send_fds(dst, [data], fds)
            else:
                dst.sendall(data)
            # send_fds() duplicated them into the other process; close ours.
            for fd in fds:
                os.close(fd)
    except OSError:
        pass
    for s in (src, dst):
        try:
            s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


def main():
    listen_path, target = sys.argv[1], sys.argv[2]
    try:
        os.unlink(listen_path)
    except FileNotFoundError:
        pass
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(listen_path)
    server.listen(4)
    while True:
        client, _ = server.accept()
        upstream = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        upstream.connect(target)
        threading.Thread(target=pump, args=(client, upstream), daemon=True).start()
        threading.Thread(target=pump, args=(upstream, client), daemon=True).start()


if __name__ == "__main__":
    main()
