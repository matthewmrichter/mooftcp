#!/usr/bin/env python3
"""Host FTP server for the Fetch and NCSA Telnet tests (docs/dev/APPS.md), unprivileged port.
    scripts/testftp.py [--bind ADDR] [port]     (default 10.0.0.1:17021; Ctrl-C stops it)
Binds the TAP host address by default (scripts/tap-up.sh; the guest's 10.0.0.1:21 is REDIRECTed
there: REDIRECT rewrites the destination to the incoming interface's address, so 127.0.0.1 would
not receive it). --bind 0.0.0.0 for a NAT-mode test (passive mode only).
Root: $MT_HOME/ftp with big10m.bin (10 MiB of deterministic pseudo-random bytes, made once;
SHA-256 printed) and incoming/. Users: anonymous (read only) and moof/moof (read, upload into
existing folders; no delete, rename or chmod). Passive ports 17100-17199. Active mode (PORT, the
only mode Fetch 2.1.2 has) connects back to the guest, which works only on the TAP link.
Needs Python >= 3.6 and pyftpdlib: python3 -m pip install --target "$MT_HOME/pylib" pyftpdlib"""
import hashlib
import socket
import logging
import os
import sys

MT_HOME = os.environ.get("MT_HOME", os.path.expanduser("~/.local/share/mooftcp"))
sys.path.insert(0, os.path.join(MT_HOME, "pylib"))
try:
    from pyftpdlib.authorizers import DummyAuthorizer
    from pyftpdlib.handlers import FTPHandler
    from pyftpdlib.handlers.ftp.dispatchers import ActiveDTP
    from pyftpdlib.servers import FTPServer
except ImportError:
    sys.exit('testftp.py: pyftpdlib missing: python3 -m pip install --target "%s/pylib" pyftpdlib'
             % MT_HOME)

args = sys.argv[1:]
bind = "10.0.0.1"
if len(args) >= 2 and args[0] == "--bind":
    bind, args = args[1], args[2:]
port = int(args[0]) if args else 17021

root = os.path.join(MT_HOME, "ftp")
os.makedirs(os.path.join(root, "incoming"), exist_ok=True)
big = os.path.join(root, "big10m.bin")
SIZE = 10 * 1024 * 1024
if not os.path.isfile(big) or os.path.getsize(big) != SIZE:
    with open(big, "wb") as f:                     # SHA-256 counter stream: same bytes everywhere
        for i in range(SIZE // 32):
            f.write(hashlib.sha256(b"mooftcp-big10m-%d" % i).digest())
h = hashlib.sha256()
with open(big, "rb") as f:
    for chunk in iter(lambda: f.read(1 << 20), b""):
        h.update(chunk)
print("big10m.bin sha256", h.hexdigest(), flush=True)

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")


class QuietActive(ActiveDTP):
    """ActiveDTP without the "200 Active data connection established" reply (PORT was answered at once)."""
    def handle_connect(self):
        self.del_channel()
        if self._idler is not None and not self._idler.cancelled:
            self._idler.cancel()
        if not self.cmd_channel.connected:
            return self.close()
        err = self.socket.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
        if err != 0:
            raise OSError(err)
        handler = self.cmd_channel.dtp_handler(self.socket, self.cmd_channel)
        self.cmd_channel.data_channel = handler
        self.cmd_channel._on_dtp_connection()


class LateHandler(FTPHandler):
    """Fetch 2.1.2 sends PORT and only then opens its listening socket, so connecting back at once (what
    pyftpdlib does) is refused even by MacTCP 2.1 (RST). This handler answers PORT at once ("200"), connects
    400 ms later (the listener is open well before Fetch's LIST, about 200 ms after its PORT) and holds every
    transfer command until that connection exists, so pyftpdlib's normal "data channel open" path runs."""
    permit_foreign_addresses = True     # test server only: the guest PORT address is the redirected peer
    active_dtp = QuietActive
    pending = False

    def _make_eport(self, ip, port):
        self.respond("200 PORT command successful.")
        self.pending = True
        self.ioloop.call_later(0.4, super()._make_eport, ip, port)

    def _held(self, name, *args):
        if self.pending:
            if self.data_channel is None:
                self.ioloop.call_later(0.05, self._held, name, *args)
                return None
            self.pending = False
        return getattr(super(), name)(*args)

    def ftp_LIST(self, path): return self._held("ftp_LIST", path)
    def ftp_NLST(self, path): return self._held("ftp_NLST", path)
    def ftp_MLSD(self, path): return self._held("ftp_MLSD", path)
    def ftp_RETR(self, f): return self._held("ftp_RETR", f)
    def ftp_STOR(self, f, mode="w"): return self._held("ftp_STOR", f, mode)
    def ftp_STOU(self, line): return self._held("ftp_STOU", line)
    def ftp_APPE(self, f): return self._held("ftp_APPE", f)


FTPHandler = LateHandler
auth = DummyAuthorizer()
auth.add_user("moof", "moof", root, perm="elrw")
auth.add_anonymous(root, perm="elr")
FTPHandler.authorizer = auth
FTPHandler.passive_ports = range(17100, 17200)
try:
    FTPServer((bind, port), FTPHandler).serve_forever()
except OSError as e:
    sys.exit("testftp.py: cannot listen on %s:%d (%s); without TAP use --bind 0.0.0.0" % (bind, port, e))
