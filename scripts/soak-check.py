#!/usr/bin/env python3
"""Summarise a soak run (scripts/soak-run.sh): MoofDiag.out snapshots, HeapInfo.out, snow.log.new.

    soak-check.py <dir>

Prints one row per MoofDiag snapshot (tick, system-heap free, gate drops, ENET tx errors/busy, core
errors and drops) and the drift between the first and last snapshot. Exit status 1 when the system
heap free changed by more than 64 bytes between the first and last snapshot (after the first open the
driver allocates nothing), when any gate drop, ENET tx error/busy/length/closed drop or core
eth_tx_err was counted, or when snow.log.new has an address error or panic.
"""
import re
import sys

d = sys.argv[1]
txt = open(d + "/MoofDiag.out").read().replace("\r", "\n")
blocks = txt.split("MoofDiag at tick ")[1:]
rows = []
for b in blocks:
    r = {"tick": int(b.split()[0])}
    m = re.search(r"system zone after open: size (\d+) free (\d+) occupied (\d+)", b)
    if m:
        r["free"], r["occ"] = int(m.group(2)), int(m.group(3))
    m = re.search(r"gate: rx (\d+) deferred (\d+) ticks (\d+) \(deferred (\d+)\) after (\d+) drops (\d+) max ring (\d+) max depth (\d+)", b)
    if m:
        r["rx"], r["gdefer"], r["gdrops"], r["maxring"] = int(m.group(1)), int(m.group(2)), int(m.group(6)), int(m.group(7))
    m = re.search(r"tx ok (\d+) pad (\d+) busy (\d+) len (\d+) closed (\d+) err (\d+)", b)
    if m:
        r["txok"], r["txbusy"], r["txlen"], r["txclosed"], r["txerr"] = (int(m.group(1)), int(m.group(3)), int(m.group(4)),
                                                                          int(m.group(5)), int(m.group(6)))
    m = re.search(r"core: eth_in (\d+) eth_out (\d+) tx_err (\d+) .* tcp_in (\d+) tcp_out (\d+) rexmt (\d+) udp_in (\d+) udp_out (\d+)", b)
    if m:
        r["eth_in"], r["eth_out"], r["ctxerr"], r["tcp_in"], r["tcp_out"], r["rexmt"] = [int(m.group(i)) for i in range(1, 7)]
    m = re.search(r"core drops: arp (\d+) ip_in (\d+) reasm_fail (\d+) udp (\d+) noport (\d+) tcp_in (\d+) fast_rexmt (\d+) rst_in (\d+) rst_out (\d+); conns_open (\d+)", b)
    if m:
        (r["arp_drops"], r["ip_drops"], r["reasm"], r["udp_drops"], r["noport"], r["tcp_drops"], r["fast"], r["rst_in"], r["rst_out"],
         r["open"]) = [int(m.group(i)) for i in range(1, 11)]
    rows.append(r)
if not rows:
    print("FAIL no MoofDiag snapshots")
    sys.exit(1)
cols = ["tick", "free", "gdrops", "maxring", "txok", "txbusy", "txerr", "ctxerr", "tcp_in", "tcp_out", "rexmt", "fast", "ip_drops",
        "tcp_drops", "udp_drops", "noport", "rst_in", "rst_out", "open"]
print(" ".join("%9s" % c for c in cols))
for r in rows:
    print(" ".join("%9s" % r.get(c, "-") for c in cols))
ok = True
first, last = rows[0], rows[-1]
if len(rows) > 1 and "free" in first and "free" in last:
    delta = last["free"] - first["free"]
    print("system heap free: first %d last %d drift %+d" % (first["free"], last["free"], delta))
    if abs(delta) > 64:
        print("FAIL system heap drift")
        ok = False
bad = [(k, last.get(k, 0)) for k in ("gdrops", "txbusy", "txlen", "txclosed", "txerr", "ctxerr") if last.get(k, 0)]
if bad:
    print("FAIL counters nonzero at the end:", bad)
    ok = False
try:
    hp = [l.split() for l in open(d + "/HeapInfo.out").read().replace("\r", "\n").split("\n") if l.startswith("HEAP")]
    print("HeapInfo FreeMem per cycle:", [int(h[1]) for h in hp], "MaxMem:", [int(h[2]) for h in hp])
    if hp and abs(int(hp[-1][1]) - int(hp[0][1])) > 64:
        print("FAIL HeapInfo FreeMem drift")
        ok = False
except OSError:
    print("no HeapInfo.out")
try:
    log = open(d + "/snow.log.new", errors="replace").read()
    lines = [l for l in log.splitlines() if re.search(r"address error|panic|bus error", l, re.I)]
    print("snow.log: %d new lines, %d address/bus/panic lines" % (len(log.splitlines()), len(lines)))
    for l in lines[:5]:
        print("   ", l)
    if lines:
        ok = False
except OSError:
    pass
print("RESULT %s" % ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
