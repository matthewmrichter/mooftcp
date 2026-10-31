#!/usr/bin/env python3
"""Interactive-TCP timing from a Snow capture (Task 13); needs tcpdump only to read the pcap.

    scripts/pcap-lat.py <file.pcap> [port]         port = the server port, default 17007

For the one connection to <port> it pairs frames by TCP sequence numbers and prints, in milliseconds
on the capture clock (Snow's emulated clock, so intervals inside one capture are meaningful; do not
compare with host clocks):
  - for every data segment the guest sent: how long until the first frame from the server that ACKs
    it (the echo also carries the ACK) and until the server's echo data arrives
  - for every data segment the server sent: how long until the guest's ACK left (our ACK delay), and
    whether that ACK was an immediate one (< 20 ms), within a delayed-ACK window, or late
  - the count of guest frames and of pure ACKs, and the segment-size histogram of the guest's data
Keystroke-to-frame (the 20 ms criterion) is NOT derivable here: the capture has no keystroke event.
"""
import re
import subprocess
import sys

pcap = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 17007
out = subprocess.run(["tcpdump", "-nn", "-tt", "-r", pcap, "tcp", "port", str(port)], capture_output=True, text=True).stdout
pat = re.compile(r"^(\d+\.\d+) IP (\S+)\.(\d+) > (\S+)\.(\d+): Flags \[([^\]]*)\](?:, seq (\d+)(?::(\d+))?)?(?:, ack (\d+))?.*length (\d+)")
fr = []
for l in out.splitlines():
    m = pat.match(l)
    if not m:
        continue
    t, sip, sport, dip, dport, fl, s1, s2, ack, ln = m.groups()
    fr.append(dict(t=float(t), guest=int(dport) == port, flags=fl, seq=int(s1) if s1 else 0, ack=int(ack) if ack else None, len=int(ln)))
if not fr:
    sys.exit("no TCP frames for port %d in %s" % (port, pcap))
t0 = fr[0]["t"]
g = [f for f in fr if f["guest"]]
h = [f for f in fr if not f["guest"]]
print("frames: %d total, %d from the guest (%d pure ACKs), %d from the server" % (
    len(fr), len(g), sum(1 for f in g if f["len"] == 0 and f["flags"] in (".", "")), len(h)))
sizes = {}
for f in g:
    if f["len"]:
        sizes[f["len"]] = sizes.get(f["len"], 0) + 1
print("guest data segment sizes:", sorted(sizes.items()))

# guest data -> server ACK / echo
ack_gaps, echo_gaps = [], []
for f in g:
    if not f["len"]:
        continue
    end = f["seq"] + f["len"]
    a = next((x for x in h if x["t"] >= f["t"] and x["ack"] is not None and (x["ack"] - end) % (1 << 32) < (1 << 31)), None)
    e = next((x for x in h if x["t"] >= f["t"] and x["len"] > 0), None)
    if a:
        ack_gaps.append((a["t"] - f["t"]) * 1000)
    if e:
        echo_gaps.append((e["t"] - f["t"]) * 1000)
# server data -> guest ACK
our = []
for f in h:
    if not f["len"]:
        continue
    end = f["seq"] + f["len"]
    a = next((x for x in g if x["t"] >= f["t"] and x["ack"] is not None and (x["ack"] - end) % (1 << 32) < (1 << 31)), None)
    if a:
        our.append((a["t"] - f["t"]) * 1000)


def summ(name, v):
    if not v:
        print("%-44s none" % name)
        return
    v = sorted(v)
    print("%-44s n=%d min %.1f median %.1f p90 %.1f max %.1f ms" % (name, len(v), v[0], v[len(v) // 2], v[int(len(v) * 0.9)], v[-1]))


summ("guest data -> first server frame ACKing it", ack_gaps)
summ("guest data -> first server data (echo)", echo_gaps)
summ("server data -> guest ACK (our ACK delay)", our)
if our:
    print("our ACKs: %d under 20 ms, %d 20-100 ms, %d over 100 ms" % (
        sum(1 for x in our if x < 20), sum(1 for x in our if 20 <= x < 100), sum(1 for x in our if x >= 100)))
# what the guest does after an echo: any frame that is not a pure ACK of it (e.g. data)? just list the first 12 exchanges
print("first frames (ms from the first, direction, flags, len):")
for f in fr[:14]:
    print("  %8.1f %s %-4s len %d" % ((f["t"] - t0) * 1000, "G>H" if f["guest"] else "H>G", f["flags"], f["len"]))
