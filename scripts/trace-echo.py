#!/usr/bin/env python3
"""trace-echo.py - per-round timing of a 1-byte echo from MoofTCP's echo-path trace (perf builds, task 18).

Input: a MoofDiag-<id>.out written by the MoofDiag of a tune+trace build (MT_TUNE=1 MT_TRACE=1). Its "TR" lines are the
driver's trace records, oldest first (format and fields: mac/glue/mt_trace_abi.h):

    trace: version 1 kept N total T cap C rec 16 lap_us L now_us U clock timemgr
    TR <t_us> <event> <a hex> <b> <c hex> <d hex>

Run MoofBatch with mode E only (64 rounds of: sync TCPSend of 1 byte with push, then sync TCPRcv), then MoofDiag; the
ring keeps the newest 768 records, about 58 rounds.

A round starts at each "PBIN Send" (the application's TCPSend entering the driver) and ends just before the next one; the
last round ends at its last "PBOUT Rcv". Frame events are kept only for TCP frames to or from --port (default 17007, the
echo service); other frames (ARP, UDP, other connections) are dropped. Each event gets a label (call and csCode, or event
and TCP flags + payload bytes), so a round reads, for example:

    PBIN Send > EWRITE AP+1 > PBOUT Send(pending) > EWDONE AP+1 > RX AP+1 > PBDONE Send > DATA +1 > EWRITE A > ...

Output: every round with the time from each event to the next (ms), then, for the most common label sequence, the median
per segment across the rounds that follow it (and min/max), and the median round time. Times are differences of the
trace clock modulo 2^32 us. Each record costs one clock read (lap_us in the header, about 0.2 ms on a Classic), which
falls inside the interval before it; --subtract-lap takes it off every interval.

Usage: trace-echo.py [--port N] [--quiet] [--subtract-lap] [--ticks] MoofDiag-<id>.out
  --quiet         no per-round listing, only the summary
  --ticks         keep TICK / TDEFER records (default: dropped, they are not part of the echo path; they are counted)
"""
import argparse
import statistics
import sys

CS = {30: "Create", 31: "PassiveOpen", 32: "ActiveOpen", 34: "Send", 35: "NoCopyRcv", 36: "RcvBfrReturn",
      37: "Rcv", 38: "Close", 39: "Abort", 40: "Status", 41: "ExtendedStat", 42: "Release", 43: "GlobalInfo",
      20: "UDPCreate", 21: "UDPRead", 22: "UDPBfrReturn", 23: "UDPWrite", 24: "UDPRelease"}
FLAGS = [(0x01, "F"), (0x02, "S"), (0x04, "R"), (0x08, "P"), (0x10, "A"), (0x20, "U")]
FRAME_EVS = {"RX", "RXRUN", "EWRITE", "EWDONE"}
MARK = {"RX": "(deferred)", "EWRITE": "(pump)", "EWDONE": "(err)", "RXRUN": ""}


class Rec:
    __slots__ = ("t", "ev", "a", "b", "c", "d", "label")

    def __init__(self, t, ev, a, b, c, d):
        self.t, self.ev, self.a, self.b, self.c, self.d = t, ev, a, b, c, d
        self.label = ""


def parse(lines):
    """Returns (header dict, list of Rec)."""
    hdr, recs = {}, []
    for ln in lines:
        p = ln.split()
        if not p:
            continue
        if p[0] == "trace:" and len(p) > 2 and p[1] == "version":
            it = iter(p[1:])
            for k in it:
                v = next(it, None)
                if v is not None:
                    hdr[k] = v
        elif p[0] == "TR" and len(p) == 7:
            try:
                recs.append(Rec(int(p[1]), p[2], int(p[3], 16), int(p[4]), int(p[5], 16), int(p[6], 16)))
            except ValueError:
                continue
    return hdr, recs


def flags(a):
    return "".join(ch for bit, ch in FLAGS if a & bit) or "-"


def label(r):
    if r.ev in ("PBIN", "PBOUT", "PBDONE"):
        name = CS.get(r.b, "cs%d" % r.b)
        s = "%s %s" % (r.ev, name)
        if r.ev == "PBOUT" and (r.d & 0xFFFF) == 1:
            s += "(pending)"
        if r.ev == "PBDONE" and r.d != 0:
            s += "(%d)" % (r.d - (1 << 32) if r.d & 0x80000000 else r.d)
        return s
    if r.ev in FRAME_EVS:
        s = r.ev + (MARK[r.ev] if r.a & 0x40 else "") + " " + flags(r.a & 0x3F)
        return s + ("+%d" % r.b if r.b else "")
    if r.ev == "DATA":
        return "DATA +%d" % r.d
    return r.ev


def keep(r, port, ticks):
    if r.ev in FRAME_EVS:
        if r.a & 0x80:                                   # not TCP
            return False
        return (r.d >> 16) == port or (r.d & 0xFFFF) == port
    if r.ev in ("TICK", "TDEFER"):
        return ticks
    return True


def rounds(recs):
    """Split into rounds at each 'PBIN Send'; the last round is cut after its last 'PBOUT Rcv'."""
    out, cur = [], None
    for r in recs:
        if r.label == "PBIN Send":
            if cur:
                out.append(cur)
            cur = [r]
        elif cur is not None:
            cur.append(r)
    if cur:
        ends = [i for i, r in enumerate(cur) if r.label.startswith("PBOUT Rcv")]
        if ends:
            out.append(cur[:ends[-1] + 1])
    return out


def dt_us(a, b):
    return (b - a) % (1 << 32)


def ms(us):
    return us / 1000.0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("file")
    ap.add_argument("--port", type=int, default=17007)
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--subtract-lap", action="store_true")
    ap.add_argument("--ticks", action="store_true")
    o = ap.parse_args(argv)
    with open(o.file, "r", encoding="latin-1", newline="") as fh:
        text = fh.read().replace("\r\n", "\n").replace("\r", "\n")
    hdr, recs = parse(text.split("\n"))
    if not recs:
        print("no TR records in %s (a MoofDiag of a tune+trace build writes them)" % o.file)
        return 1
    lap = int(hdr.get("lap_us", "0") or 0)
    nticks = sum(1 for r in recs if r.ev in ("TICK", "TDEFER"))
    for r in recs:
        r.label = label(r)
    kept = [r for r in recs if keep(r, o.port, o.ticks)]
    rs = rounds(kept)
    print("trace: %d records (%s kept by the driver, %s written), %d after the filter (port %d%s); %d timer records%s; "
          "clock read cost lap_us %d%s" % (len(recs), hdr.get("kept", "?"), hdr.get("total", "?"), len(kept), o.port,
                                          ", ticks kept" if o.ticks else "", nticks,
                                          "" if o.ticks else " dropped", lap,
                                          " (subtracted from every interval)" if o.subtract_lap else ""))
    if not rs:
        print("no echo rounds (no 'PBIN Send' ... 'PBOUT Rcv' in the records)")
        return 1

    def seg(a, b):
        d = dt_us(a.t, b.t)
        return max(0, d - lap) if o.subtract_lap else d

    totals = []
    for n, rd in enumerate(rs, 1):
        tot = sum(seg(rd[i - 1], rd[i]) for i in range(1, len(rd)))
        nxt = rs[n] if n < len(rs) else None
        cycle = seg(rd[0], nxt[0]) if nxt else None
        totals.append((tot, cycle))
        if o.quiet:
            continue
        print("round %d: %d events, send -> last event %.2f ms%s" %
              (n, len(rd), ms(tot), (", send -> next send %.2f ms" % ms(cycle)) if cycle is not None else ""))
        print("  %8s  %s" % ("", rd[0].label))
        for i in range(1, len(rd)):
            print("  %+8.2f  %s" % (ms(seg(rd[i - 1], rd[i])), rd[i].label))
        if nxt:
            print("  %+8.2f  (next PBIN Send)" % ms(seg(rd[-1], nxt[0])))

    # the most common label sequence; the last round (cut at its last "PBOUT Rcv", the connection closes after it)
    # follows it too when it is a prefix of it, and then counts for the segments it has
    pats = {}
    for n, rd in enumerate(rs):
        pats.setdefault(tuple(r.label for r in rd), []).append(n)
    full = {k: [n for n in v if n != len(rs) - 1] for k, v in pats.items()}
    seq = max(full.items(), key=lambda kv: len(kv[1]))[0] if any(full.values()) else tuple(r.label for r in rs[-1])
    last = tuple(r.label for r in rs[-1])
    idx = list(full.get(seq, []))
    if last == seq[:len(last)]:
        idx.append(len(rs) - 1)
        if last != seq:
            pats[last] = [n for n in pats[last] if n != len(rs) - 1]
            if not pats[last]:
                del pats[last]
            pats.setdefault(seq, []).append(len(rs) - 1)
    print()
    print("pattern: %d of %d rounds follow the most common sequence (%d events); %d other sequence(s)" %
          (len(idx), len(rs), len(seq), len(pats) - 1))
    print("median per segment over those rounds (ms): median  min  max  segment")
    for i in range(1, len(seq)):
        v = [ms(seg(rs[n][i - 1], rs[n][i])) for n in idx if i < len(rs[n])]
        print("  %7.2f %7.2f %7.2f  %s -> %s" % (statistics.median(v), min(v), max(v), seq[i - 1], seq[i]))
    v = [ms(seg(rs[n][-1], rs[n + 1][0])) for n in idx if n + 1 < len(rs)]
    if v:
        print("  %7.2f %7.2f %7.2f  %s -> (next PBIN Send: the application)" %
              (statistics.median(v), min(v), max(v), seq[-1]))
    idx = [n for n in idx if len(rs[n]) == len(seq)]   # whole rounds only below
    t = [ms(totals[n][0]) for n in idx]
    if t:
        print("round, send -> last event: median %.2f ms (min %.2f, max %.2f)" % (statistics.median(t), min(t), max(t)))
    c = [ms(totals[n][1]) for n in range(len(rs)) if totals[n][1] is not None]
    if c:
        print("all rounds, send -> next send: median %.2f ms over %d (the echo RTT plus the application's own time)" %
              (statistics.median(c), len(c)))
    for s, ns in sorted(pats.items(), key=lambda kv: -len(kv[1])):
        if s == seq:
            continue
        print("other sequence (%d round(s), first round %d): %s" % (len(ns), ns[0] + 1, " > ".join(s)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
