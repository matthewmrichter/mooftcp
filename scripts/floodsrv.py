#!/usr/bin/env python3
"""Host-side bulk-receive source for the soak (Task 13); Python 3 stdlib only.

    scripts/floodsrv.py [--port 17041] [--seconds 60] [--bind 0.0.0.0]

Every TCP connection first gets printable ASCII lines (CRLF, 78 characters) as fast as the guest's
window lets it for --seconds seconds, then the server echoes what it receives, so one long-lived
Flynn session sees bulk receive and then interactive typing on the same connection. The source
is a plain socket: when the guest advertises a zero window the send blocks (flow control, no drops).
Logs connections, the byte count when the flood ends and closes to stdout. SIGINT/SIGTERM stops it.
"""
import argparse
import datetime
import signal
import socket
import socketserver
import sys
import threading
import time


def log(msg):
    print(datetime.datetime.now().isoformat(timespec="milliseconds"), msg, flush=True)


LINE = (b"0123456789abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ .,;:-_+*/ 0123456789"[:78]) + b"\r\n"


class Flood(socketserver.BaseRequestHandler):
    seconds = 60

    def handle(self):
        peer = "%s:%d" % self.client_address[:2]
        s = self.request
        log("connect from " + peer)
        chunk = LINE * 12
        sent = 0
        end = time.time() + self.seconds
        s.settimeout(5.0)
        try:
            while time.time() < end:
                try:
                    s.sendall(chunk)
                    sent += len(chunk)
                except socket.timeout:
                    pass          # window closed for 5 s: keep trying until the flood time is up
            log("flood over for %s after %d bytes" % (peer, sent))
            s.settimeout(None)
            got = 0
            while True:
                data = s.recv(4096)
                if not data:
                    break
                got += len(data)
                s.sendall(data)
            log("closed by %s after echoing %d bytes" % (peer, got))
        except (OSError, ConnectionError) as e:
            log("error with %s: %s" % (peer, e))


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=17041)
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--bind", default="0.0.0.0")
    a = ap.parse_args()
    Flood.seconds = a.seconds
    srv = Server((a.bind, a.port), Flood)
    signal.signal(signal.SIGTERM, lambda *_: threading.Thread(target=srv.shutdown).start())
    log("flood server on %s:%d (%d s flood)" % (a.bind, a.port, a.seconds))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    srv.server_close()


if __name__ == "__main__":
    sys.exit(main())
