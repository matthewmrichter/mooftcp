#!/usr/bin/env python3
"""Host side of KeyLat (mac/keylat): keypress-to-wire latency of a real Mac, MoofTCP or MacTCP. Python 3 stdlib only.

    scripts/keylatsrv.py [--port 17061] [--out base]            # owner: run it, then start KeyLat on the Mac
    scripts/keylatsrv.py --report base.host.txt [--mac KeyLat.out]   # redo the analysis later, with the Mac's log
    scripts/keylatsrv.py --inject N [--display :142]            # Snow test: type N keys into the Snow window (XTest)

The Mac sends one small line per event ("KL1 K seq when seen" per keyDown, "KL1 S seq tick" per calibration ping);
this script stamps each line with its own clock (time.perf_counter_ns at recv() return) and, at the end of the
connection (or Ctrl-C), prints the statistics and writes <out>.host.txt (raw lines) and <out>.csv.

What is measured, and how accurate it is (the numbers are honest about the Mac's 60.15 Hz clock):
  * The Mac's only clock is TickCount, 1 tick = 16.6 ms. EventRecord.when is the tick at which the keyboard event
    was posted: the real key press happened up to one tick (plus the keyboard's own poll delay, not visible to
    software) EARLIER. "age" (seen - when), "call" and "ack" below are whole ticks.
  * Calibration pings are sent by the app the instant TickCount changes to <tick> (busy-wait for the edge), so a
    ping leaves within a fraction of a millisecond of the start of tick <tick>. The lower convex hull of
    (tick, host arrival) over all pings is the baseline: the host time at which an idle ping of a tick would
    arrive (pings only ever arrive late, never early, so the minimum is the tight one). It also gives the drift
    and the tick rate (a real Mac: 60.15 Hz; Snow runs fast).
  * L_est = arrival(key) - baseline(when): event posted -> packet at the host, MINUS the best-case path of an idle
    ping (that path, stack + wire, is typically 1-3 ms and is not subtracted from the true latency).
    The true event-posted -> packet-arrived latency lies in [L_est - 1 tick, L_est + OWD] (OWD = --owd-ms, default
    2): `when` is rounded down to a tick, so L_est can read up to 16.6 ms too high.
  * Verdict against the goal (default 20 ms): "sure pass" = L_est + OWD < goal; "sure fail" = L_est - 1 tick >=
    goal; the rest is "inconclusive" (the clock cannot tell). With the Mac's resolution only a clear margin counts.
"""
import argparse
import re
import socket
import sys
import time

NOMINAL_HZ = 60.147          # the Mac's tick rate (documented 60.15)
LINE_RE = re.compile(rb"^KL1 ([KS]) (\d+) (\d+)(?: (\d+))?$")


def parse_line(line):
    """b'KL1 K seq when seen' -> ('K', seq, when, seen); b'KL1 S seq tick' -> ('S', seq, tick, None); else None."""
    m = LINE_RE.match(line.strip(b"\r\n \x00"))
    if not m:
        return None
    seen = int(m.group(4)) if m.group(4) is not None else None
    if m.group(1) == b"K" and seen is None:
        return None
    return (m.group(1).decode(), int(m.group(2)), int(m.group(3)), seen)


class LineBuf:
    """Splits a TCP byte stream into lines; feed() returns the complete ones."""

    def __init__(self):
        self.buf = b""

    def feed(self, data):
        self.buf += data
        parts = self.buf.split(b"\n")
        self.buf = parts.pop()
        if len(self.buf) > 4096:
            self.buf = b""
        return parts


def lower_hull(pts):
    """Lower convex hull of (x, y) points sorted by x (duplicates in x keep the lowest y)."""
    best = {}
    for x, y in pts:
        if x not in best or y < best[x]:
            best[x] = y
    p = sorted(best.items())
    h = []
    for q in p:
        while len(h) >= 2 and (h[-1][0] - h[-2][0]) * (q[1] - h[-2][1]) - (h[-1][1] - h[-2][1]) * (q[0] - h[-2][0]) <= 0:
            h.pop()
        h.append(q)
    return h


class Baseline:
    """Host time (ms) of the start of Mac tick x, from the sync pings [(tick, arrival_ms)]."""

    def __init__(self, syncs, tick_ms=1000.0 / NOMINAL_HZ):
        self.tick_ms = tick_ms
        self.hull = lower_hull(syncs)
        self.n = len(syncs)

    def ok(self):
        return len(self.hull) >= 1

    def rate_hz(self):
        """Observed tick rate from the hull's end points (None if the pings span < 20 s of ticks)."""
        if len(self.hull) < 2 or self.hull[-1][0] - self.hull[0][0] < 1200:
            return None
        (x0, y0), (x1, y1) = self.hull[0], self.hull[-1]
        return 1000.0 * (x1 - x0) / (y1 - y0)

    def at(self, x):
        h = self.hull
        if len(h) == 1:
            return h[0][1] + (x - h[0][0]) * self.tick_ms
        if x <= h[0][0]:
            a, b = h[0], h[1]
        elif x >= h[-1][0]:
            a, b = h[-2], h[-1]
        else:
            a, b = h[0], h[1]
            for i in range(len(h) - 1):
                if h[i][0] <= x <= h[i + 1][0]:
                    a, b = h[i], h[i + 1]
                    break
        return a[1] + (x - a[0]) * (b[1] - a[1]) / (b[0] - a[0])


def analyse(events, goal_ms=20.0, owd_ms=2.0, tick_ms=1000.0 / NOMINAL_HZ):
    """events: [(kind, seq, a, b, arrival_ms)]. Returns a dict with the per-key rows and the summary."""
    syncs = [(a, t) for (k, s, a, b, t) in events if k == "S"]
    keys = [(s, a, b, t) for (k, s, a, b, t) in events if k == "K"]
    base = Baseline(syncs, tick_ms)
    out = {"nsync": len(syncs), "nkeys": len(keys), "rows": [], "base": base}
    if not base.ok():
        return out
    t0 = base.hull[0][1]
    res = sorted(t - base.at(a) for (a, t) in syncs)
    out["cal_median"], out["cal_max"] = res[len(res) // 2], res[-1]
    rows = []
    for s, when, seen, t in keys:
        rows.append({"seq": s, "when": when, "seen": seen, "age": seen - when, "arr_ms": t - t0,
                     "L": t - base.at(when)})
    out["rows"] = rows
    lat = sorted(r["L"] for r in rows)
    if lat:
        n = len(lat)
        out["min"], out["max"] = lat[0], lat[-1]
        out["p50"] = lat[n // 2]
        out["p90"] = lat[min(n - 1, int(n * 0.9))]
        out["mean"] = sum(lat) / n
        out["pass"] = sum(1 for x in lat if x + owd_ms < goal_ms)
        out["fail"] = sum(1 for x in lat if x - tick_ms >= goal_ms)
        out["inconclusive"] = n - out["pass"] - out["fail"]
        out["neg"] = sum(1 for x in lat if x < -owd_ms)
    return out


def hist(vals):
    c = {}
    for v in vals:
        c[v] = c.get(v, 0) + 1
    return "  ".join("%d:%d" % (k, c[k]) for k in sorted(c))


def parse_mac_log(text):
    """KeyLat.out (CR or LF lines) -> {seq: dict} for K lines: when seen call ret done res ch."""
    d = {}
    for ln in re.split(r"[\r\n]+", text):
        f = ln.split()
        if len(f) >= 8 and f[0] == "K":
            d[int(f[1])] = {"when": int(f[2]), "seen": int(f[3]), "call": int(f[4]), "ret": int(f[5]),
                            "done": int(f[6]), "res": int(f[7]), "ch": f[8] if len(f) > 8 else ""}
    return d


def report(events, mac=None, goal_ms=20.0, owd_ms=2.0, csv=None, tick_ms=1000.0 / NOMINAL_HZ):
    r = analyse(events, goal_ms, owd_ms, tick_ms)
    L = []
    L.append("KeyLat report: %d key packets, %d calibration pings" % (r["nkeys"], r["nsync"]))
    base = r["base"]
    if not base.ok():
        L.append("no calibration pings arrived: cannot place the keys on the host clock")
        return "\n".join(L), r
    hz = base.rate_hz()
    L.append("Mac tick rate seen from the pings: %s (nominal %.2f Hz; one tick = %.1f ms)" % (
        "%.3f Hz" % hz if hz else "n/a (pings span under 20 s)", NOMINAL_HZ, tick_ms))
    if not r["rows"]:
        L.append("no key packets")
        return "\n".join(L), r
    L.append("calibration spread (pings above the baseline): median %.1f ms, max %.1f ms%s" % (
        r["cal_median"], r["cal_max"],
        "  WARNING: noisy link or clock, L_est is only a rough figure" if r["cal_median"] > 5.0 else ""))
    ages = [x["age"] for x in r["rows"]]
    L.append("Mac, whole ticks (exact): age = event seen - event.when: " + hist(ages))
    if mac:
        m = [mac[x["seq"]] for x in r["rows"] if x["seq"] in mac]
        if m:
            L.append("Mac, whole ticks (exact, %d keys in KeyLat.out): send call start - when: %s" % (
                len(m), hist([x["call"] - x["when"] for x in m])))
            L.append("                                     TCPSend call duration: %s" % hist([x["ret"] - x["call"] for x in m]))
            L.append("                                     send completed (acked) - call: %s" % hist(
                [x["done"] - x["call"] for x in m if x["done"]]))
            for x in r["rows"]:
                if x["seq"] in mac:
                    x.update(call=mac[x["seq"]]["call"], ret=mac[x["seq"]]["ret"], done=mac[x["seq"]]["done"])
    n = len(r["rows"])
    L.append("L_est (event posted -> packet at host, minus an idle ping's path), ms, resolution one tick:")
    L.append("  min %.1f  p50 %.1f  mean %.1f  p90 %.1f  max %.1f" % (r["min"], r["p50"], r["mean"], r["p90"], r["max"]))
    L.append("  true latency of a key lies in [L_est - %.1f, L_est + %.1f] ms" % (tick_ms, owd_ms))
    L.append("Goal %.0f ms: sure pass %d of %d, inconclusive %d, sure fail %d" % (
        goal_ms, r["pass"], n, r["inconclusive"], r["fail"]))
    if r["neg"]:
        L.append("WARNING: %d keys with L_est below -%.0f ms: the calibration pings did not cover this run" % (r["neg"], owd_ms))
    L.append("Not visible to any software on the Mac: the keyboard's own delay before the event is posted.")
    if csv:
        with open(csv, "w") as f:
            f.write("seq,when,seen,age_ticks,L_est_ms,call,ret,done\n")
            for x in r["rows"]:
                f.write("%d,%d,%d,%d,%.2f,%s,%s,%s\n" % (x["seq"], x["when"], x["seen"], x["age"], x["L"],
                                                          x.get("call", ""), x.get("ret", ""), x.get("done", "")))
    return "\n".join(L), r


def read_raw(path):
    ev = []
    with open(path) as fh:
        for ln in fh:
            f = ln.split()
            if len(f) == 5:
                ev.append((f[0], int(f[1]), int(f[2]), None if f[3] == "-" else int(f[3]), float(f[4])))
    return ev


def inject_keys(display, nkeys, gap_ms, wait_events):
    """Type nkeys lowercase letters into the X display with XTest, gap_ms..1.5x gap_ms apart."""
    import ctypes
    import ctypes.util
    import random
    x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
    xtst = ctypes.CDLL(ctypes.util.find_library("Xtst"))
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    x11.XFlush.argtypes = [ctypes.c_void_p]
    xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
    dpy = x11.XOpenDisplay(display.encode())
    if not dpy:
        sys.exit("cannot open display " + display)
    wait_events()
    for i in range(nkeys):
        kc = x11.XKeysymToKeycode(dpy, ord("abcdefghijklmnopqrstuvwxyz"[i % 26]))
        xtst.XTestFakeKeyEvent(dpy, kc, 1, 0)
        x11.XFlush(dpy)
        time.sleep(0.04)
        xtst.XTestFakeKeyEvent(dpy, kc, 0, 0)
        x11.XFlush(dpy)
        time.sleep(random.uniform(gap_ms, gap_ms * 1.5) / 1000.0)


def serve(a):
    import threading
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", a.port))
    srv.listen(1)
    events = []
    lock = threading.Lock()
    done = threading.Event()
    ready = threading.Event()
    stats = {"bad": 0, "last": time.time()}
    t_base = time.perf_counter_ns()

    def conn_thread():
        c, peer = srv.accept()
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print("KeyLat connected from %s:%d. Calibrating (about 5 s), then type on the Mac." % peer, flush=True)
        lb = LineBuf()
        nk = 0
        while True:
            try:
                d = c.recv(4096)
            except OSError:
                break
            t = time.perf_counter_ns()
            if not d:
                break
            stats["last"] = time.time()
            for ln in lb.feed(d):
                p = parse_line(ln)
                if not p:
                    stats["bad"] += 1
                    continue
                k, s, x, y = p
                with lock:
                    events.append((k, s, x, y, t / 1e6))
                    if k == "S" and sum(1 for e in events if e[0] == "S") == 20:
                        print("calibrated: type now (Cmd-Q on the Mac ends the run)", flush=True)
                        ready.set()
                if k == "K":
                    nk += 1
                    with lock:
                        b = Baseline([(e[2], e[4]) for e in events if e[0] == "S"])
                    msg = "key %3d  seq %d  when %d  age %d ticks" % (nk, s, x, y - x)
                    if b.ok():
                        msg += "  L_est %.1f ms" % (t / 1e6 - b.at(x))
                    print(msg, flush=True)
        done.set()

    threading.Thread(target=conn_thread, daemon=True).start()
    print("waiting for KeyLat on port %d (host addresses: run `hostname -I`)" % a.port, flush=True)
    try:
        if a.inject:
            t_wait = time.time()
            while not ready.wait(0.5):
                if done.is_set() or time.time() - t_wait > a.idle_timeout * 4:
                    print("no calibrated KeyLat connection: giving up", flush=True)
                    break
            if not ready.is_set():
                raise KeyboardInterrupt
        if a.inject:
            time.sleep(1.0)
            inject_keys(a.display, a.inject, a.gap_ms, lambda: None)
            print("injected %d keys; waiting for the end of the run" % a.inject, flush=True)
        while not done.wait(0.5):
            if a.inject and time.time() - stats["last"] > a.idle_timeout:
                break
    except KeyboardInterrupt:
        print("\nstopped", flush=True)
    with lock:
        ev = list(events)
    if a.out:
        with open(a.out + ".host.txt", "w") as f:
            for k, s, x, y, t in ev:
                f.write("%s %d %d %s %.4f\n" % (k, s, x, "-" if y is None else y, t))
    text, _ = report(ev, None, a.goal_ms, a.owd_ms, a.out + ".csv" if a.out else None)
    print(text)
    if stats["bad"]:
        print("(%d unparsable lines ignored)" % stats["bad"])
    return ev


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=17061)
    ap.add_argument("--out", help="base name: writes <out>.host.txt (raw) and <out>.csv")
    ap.add_argument("--goal-ms", type=float, default=20.0)
    ap.add_argument("--owd-ms", type=float, default=2.0, help="allowance for an idle ping's own path (default 2)")
    ap.add_argument("--report", metavar="HOST.TXT", help="analyse a saved <out>.host.txt instead of serving")
    ap.add_argument("--mac", metavar="KeyLat.out", help="with --report: add the Mac's own tick log")
    ap.add_argument("--inject", type=int, default=0, metavar="N", help="Snow test: type N keys into --display")
    ap.add_argument("--display", default=":142")
    ap.add_argument("--gap-ms", type=int, default=600)
    ap.add_argument("--idle-timeout", type=float, default=30.0)
    a = ap.parse_args()
    if a.report:
        mac = parse_mac_log(open(a.mac, errors="replace").read()) if a.mac else None
        text, _ = report(read_raw(a.report), mac, a.goal_ms, a.owd_ms, a.out + ".csv" if a.out else None)
        print(text)
        return
    serve(a)


if __name__ == "__main__":
    main()
