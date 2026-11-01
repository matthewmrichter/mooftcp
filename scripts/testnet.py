#!/usr/bin/env python3
"""Host-side TCP test services for MoofTCP (Python 3 stdlib only).

    scripts/testnet.py [--bind 0.0.0.0] [--base 17000] [--only 13,19,37]

    base+7   echo     returns every byte it receives
    base+9   discard  reads and drops everything
    base+23  banner   sends "MoofTCP test host\\r\\nlogin: ", then echoes
    base+13  oneshot  sends one line, then closes (clean FIN)
    base+19  burst    sends 5000 bytes (pattern A..Z), then closes (clean FIN)
    base+37  reset    sends "x", then closes with RST
    base+7/udp, base+9/udp   UDP echo / UDP discard (used by mac/apiprobe)
    "LATE" on the TCP or UDP echo: instead of echoing at once, the host waits --late-ms (default 3000), then sends 20 x 200 bytes
    (TCP; the connection stays open) or echoes the datagram (UDP): data that arrives after a MultiFinder
    application quit with reads pending (mac/mfecho option P, Plan 3 Task 12)

Every connection, close and error is logged to stdout with a timestamp.
Ctrl-C (SIGINT) or SIGTERM stops it.
"""
import argparse
import datetime
import signal
import socket
import socketserver
import sys
import struct
import threading

BANNER = b"MoofTCP test host\r\nlogin: "
LATE_S = 3.0      # --late-ms


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
                if self.echo and data.startswith(b"LATE"):
                    sock = self.request

                    def late():
                        try:
                            for _ in range(20):
                                sock.sendall(b"L" * 200)
                            log(f"{self.name}:{port} {peer} late burst sent (4000 bytes)")
                        except OSError as e:
                            log(f"{self.name}:{port} {peer} late burst error {e}")
                    threading.Timer(LATE_S, late).start()
                elif self.echo:
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


class Oneshot(Service):
    name = "oneshot"
    banner = b"MoofTCP oneshot\r\n"

    def handle(self):
        peer = "%s:%d" % self.client_address[:2]
        log(f"oneshot connect from {peer}")
        try:
            self.request.sendall(self.banner)
        except OSError as e:
            log(f"oneshot {peer} error {e}")
        log(f"oneshot close to {peer}")


class Burst(Service):
    name = "burst"

    def handle(self):
        peer = "%s:%d" % self.client_address[:2]
        log(f"burst connect from {peer}")
        data = bytes(65 + (i % 26) for i in range(5000))
        try:
            self.request.sendall(data)
        except OSError as e:
            log(f"burst {peer} error {e}")
        log(f"burst close to {peer} (5000 bytes out)")


class Reset(Service):
    name = "reset"

    def handle(self):
        peer = "%s:%d" % self.client_address[:2]
        log(f"reset connect from {peer}")
        try:
            self.request.sendall(b"x")
            self.request.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                    struct.pack("ii", 1, 0))
        except OSError as e:
            log(f"reset {peer} error {e}")
        log(f"reset RST to {peer}")


def udp_service(bind, port, echo):
    """UDP echo/discard. Replies are sent from the address the datagram was sent to
    (IP_PKTINFO): the host has several addresses, and Snow's NAT drops a reply whose
    source differs from the destination the guest used (172.17.0.1)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_PKTINFO, 1)
    sock.bind((bind, port))

    def loop():
        while True:
            try:
                data, anc, _flags, addr = sock.recvmsg(65535, 256)
            except OSError:
                return
            log(f"udp:{port} {addr[0]}:{addr[1]} recv {len(data)} bytes")
            if not echo:
                continue
            ctl = []
            for level, typ, cdata in anc:
                if level == socket.IPPROTO_IP and typ == socket.IP_PKTINFO and len(cdata) >= 12:
                    # in_pktinfo: ifindex, spec_dst, addr (the header destination)
                    ctl = [(socket.IPPROTO_IP, socket.IP_PKTINFO,
                            struct.pack("I4s4s", 0, cdata[8:12], b"\0\0\0\0"))]
            def send(data=data, ctl=ctl, addr=addr):
                try:
                    sock.sendmsg([data], ctl, 0, addr)
                    log(f"udp:{port} {addr[0]}:{addr[1]} sent {len(data)} bytes")
                except OSError as e:
                    log(f"udp:{port} send error {e}")
            if data.startswith(b"LATE"):
                threading.Timer(LATE_S, send).start()
            else:
                send()

    threading.Thread(target=loop, daemon=True).start()
    return sock


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bind", default="0.0.0.0", help="address to listen on (default 0.0.0.0)")
    ap.add_argument("--base", type=int, default=17000, help="base port (default 17000)")
    ap.add_argument("--only", default="", help="comma list of port offsets to serve (e.g. 13,19,37); default all")
    ap.add_argument("--late-ms", type=int, default=3000, help="delay of the answer to LATE (default 3000)")
    args = ap.parse_args()
    global LATE_S
    LATE_S = args.late_ms / 1000.0
    only = {int(x) for x in args.only.split(",") if x}

    servers = []
    for offset, handler in ((7, Echo), (9, Discard), (23, Banner), (13, Oneshot),
                            (19, Burst), (37, Reset)):
        if only and offset not in only:
            continue
        srv = Server((args.bind, args.base + offset), handler)
        servers.append(srv)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        log(f"{handler.name} listening on {args.bind}:{args.base + offset}")

    for offset, echo in ((7, True), (9, False)):
        if only and offset not in only:
            continue
        servers.append(udp_service(args.bind, args.base + offset, echo))
        log(f"udp {'echo' if echo else 'discard'} listening on {args.bind}:{args.base + offset}")

    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    stop.wait()
    for srv in servers:
        if isinstance(srv, socket.socket):
            srv.close()
        else:
            srv.shutdown()
            srv.server_close()
    log("stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
