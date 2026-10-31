#!/usr/bin/env python3
"""Keystroke-to-host latency probe for the Snow TAP link (Task 13); Python 3 stdlib + libX11/libXtst via ctypes.

    scripts/latsrv.py [--port 17051] [--keys 60] [--display :142] [--gap-ms 600] [--out file.csv]

Runs an echo server and, once a guest connects (Flynn, host 10.0.0.1 port 17051), injects --keys single
key presses into the Snow window with XTest (key down, key up 40 ms later, random gap of gap-ms..1.5x gap-ms between
keys) and timestamps on ONE clock (CLOCK_REALTIME ns on this host):
    t_inject   just before the XTest request is flushed to the X server
    t_arrive   the server's recv() returned the keystroke segment (it is written back at once: t_echo)
It prints per key inject->arrive in ms and summary statistics. What it proves: total time from the host key event
through Snow's input path, the guest's keyboard handling, Flynn's event loop, the stack and the emulated Ethernet to
the host socket, on one clock, so MoofTCP and MacTCP runs can be compared with the same tool. What it does not
prove: the stack's own share (no frame timestamps inside the guest), nor the time on a real 68000 (Snow's guest clock
runs about 1.5x faster than wall time and Snow is not cycle-throttled to a Classic).
"""
import argparse
import ctypes
import ctypes.util
import random
import socket
import statistics
import sys
import threading
import time

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=17051)
ap.add_argument("--keys", type=int, default=60)
ap.add_argument("--display", default=":142")
ap.add_argument("--gap-ms", type=int, default=600)
ap.add_argument("--out")
a = ap.parse_args()

x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
xtst = ctypes.CDLL(ctypes.util.find_library("Xtst"))
x11.XOpenDisplay.restype = ctypes.c_void_p
x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x11.XFlush.argtypes = [ctypes.c_void_p]
xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
dpy = x11.XOpenDisplay(a.display.encode())
if not dpy:
    sys.exit("cannot open display " + a.display)
keysyms = [ord(c) for c in "abcdefghijklmnopqrstuvwxyz"]

srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", a.port))
srv.listen(2)
arrivals = []          # (t_arrive_ns, nbytes)
lock = threading.Lock()
connected = threading.Event()


def serve():
    c, peer = srv.accept()
    c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print("connected from %s:%d" % peer, flush=True)
    connected.set()
    while True:
        d = c.recv(4096)
        t = time.time_ns()
        if not d:
            break
        c.sendall(d)
        with lock:
            arrivals.append((t, len(d), time.time_ns()))


threading.Thread(target=serve, daemon=True).start()
print("waiting for the guest on port %d" % a.port, flush=True)
connected.wait()
time.sleep(3)
injects = []
for i in range(a.keys):
    ks = keysyms[i % len(keysyms)]
    kc = x11.XKeysymToKeycode(dpy, ks)
    t0 = time.time_ns()
    xtst.XTestFakeKeyEvent(dpy, kc, 1, 0)
    x11.XFlush(dpy)
    injects.append(t0)
    time.sleep(0.04)
    xtst.XTestFakeKeyEvent(dpy, kc, 0, 0)
    x11.XFlush(dpy)
    time.sleep(random.uniform(a.gap_ms, a.gap_ms * 1.5) / 1000.0)
time.sleep(2)
with lock:
    arr = list(arrivals)
lat = []
rows = []
j = 0
for i, t0 in enumerate(injects):
    while j < len(arr) and arr[j][0] < t0:
        j += 1
    if j >= len(arr):
        break
    nxt = injects[i + 1] if i + 1 < len(injects) else t0 + 10**10
    if arr[j][0] >= nxt:
        rows.append((i, None))     # no segment before the next key: lost or coalesced
        continue
    ms = (arr[j][0] - t0) / 1e6
    lat.append(ms)
    rows.append((i, ms))
    j += 1
lat.sort()
print("keys injected %d, matched %d, unmatched %d, segments seen %d" % (len(injects), len(lat), len(rows) - len(lat), len(arr)))
if lat:
    print("inject -> host arrival (ms): min %.1f p10 %.1f median %.1f mean %.1f p90 %.1f max %.1f stdev %.1f" % (
        lat[0], lat[len(lat) // 10], statistics.median(lat), statistics.mean(lat), lat[int(len(lat) * 0.9)], lat[-1],
        statistics.pstdev(lat)))
    print("under 20 ms: %d of %d (%.0f%%)" % (sum(1 for x in lat if x < 20), len(lat), 100.0 * sum(1 for x in lat if x < 20) / len(lat)))
if a.out:
    with open(a.out, "w") as f:
        for i, ms in rows:
            f.write("%d,%s\n" % (i, "" if ms is None else "%.2f" % ms))
