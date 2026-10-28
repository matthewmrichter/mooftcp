#!/usr/bin/env python3
"""Host-side TCP test services for MoofTCP (Python 3 stdlib only).

    scripts/testnet.py [--bind 0.0.0.0] [--base 17000]

    base+7   echo     returns every byte it receives
    base+9   discard  reads and drops everything
    base+23  banner   sends "MoofTCP test host\\r\\nlogin: ", then echoes

Every connection, close and error is logged to stdout with a timestamp.
Ctrl-C (SIGINT) or SIGTERM stops it.
"""
import argparse
import datetime
import signal
import socketserver
import sys
import threading

BANNER = b"MoofTCP test host\r\nlogin: "


def log(msg):
    ts = datetime.datetime.now().isoformat(timespec="milliseconds")
    print(f"{ts} {msg}", flush=True)


class Service(socketserver.BaseRequestHandler):
    name = "?"
    banner = b""
    echo = True

    def handle(self):
        peer = "%s:%d" % self.client_address[:2]
        port = self.server.server_address[1]
        log(f"{self.name}:{port} connect from {peer}")
        total = 0
        try:
            if self.banner:
                self.request.sendall(self.banner)
            while True:
                data = self.request.recv(4096)
                if not data:
                    break
                total += len(data)
                log(f"{self.name}:{port} {peer} recv {len(data)} bytes {data!r}")
                if self.echo:
                    self.request.sendall(data)
        except OSError as e:
            log(f"{self.name}:{port} {peer} error {e}")
        log(f"{self.name}:{port} close from {peer} ({total} bytes in)")


class Echo(Service):
    name = "echo"


class Discard(Service):
    name = "discard"
    echo = False


class Banner(Service):
    name = "banner"
    banner = BANNER


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bind", default="0.0.0.0", help="address to listen on (default 0.0.0.0)")
    ap.add_argument("--base", type=int, default=17000, help="base port (default 17000)")
    args = ap.parse_args()

    servers = []
    for offset, handler in ((7, Echo), (9, Discard), (23, Banner)):
        srv = Server((args.bind, args.base + offset), handler)
        servers.append(srv)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        log(f"{handler.name} listening on {args.bind}:{args.base + offset}")

    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    stop.wait()
    for srv in servers:
        srv.shutdown()
        srv.server_close()
    log("stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
