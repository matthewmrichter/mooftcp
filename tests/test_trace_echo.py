#!/usr/bin/env python3
"""scripts/trace-echo.py (task 18): parsing MoofDiag's TR lines, the rounds, the medians per segment. ctest runs it."""
import contextlib
import importlib.util
import io
import os
import tempfile
import unittest

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("trace_echo", os.path.join(here, "..", "scripts", "trace-echo.py"))
te = importlib.util.module_from_spec(spec)
spec.loader.exec_module(te)

PORTS = (1025 << 16) | 17007       # our port -> the echo port (EWRITE)
RPORTS = (17007 << 16) | 1025      # the echo -> us (RX)


def round_lines(t, gap_ack_to_rcv):
    """One echo round as the driver records it (direct EWrites), starting at t; returns (lines, end t)."""
    ev = [
        (0, "PBIN", 0x00, 34, 0x100, 0),
        (300, "EWRITE", 0x18, 1, 0x1000, PORTS),
        (250, "PBOUT", 0x00, 34, 0x100, 1),
        (6000, "EWDONE", 0x18, 1, 0x1000, PORTS),
        (500, "TICK", 0x00, 0, 0x55, 0),                 # dropped by default
        (9000, "RX", 0x18, 1, 0x2000, RPORTS),
        (300, "PBDONE", 0x00, 34, 0x100, 0),
        (250, "DATA", 0x00, 0, 0x2000, 1),
        (300, "EWRITE", 0x10, 0, 0x1001, PORTS),
        (gap_ack_to_rcv, "PBIN", 0x00, 37, 0x200, 0),
        (300, "PBDONE", 0x00, 37, 0x200, 0),
        (250, "PBOUT", 0x00, 37, 0x200, 0),
        (5000, "EWDONE", 0x10, 0, 0x1001, PORTS),
        (100, "RX", 0x80, 60, 0x08060000, 0),             # ARP: dropped
    ]
    out = []
    for dt, name, a, b, c, d in ev:
        t = (t + dt) % (1 << 32)
        out.append("TR %d %s %02x %d %08x %08x" % (t, name, a, b, c, d))
    return out, t


def diag_out(n, start=1000, gaps=None):
    lines = ["MoofDiag at tick 1", "trace: version 1 kept 99 total 99 cap 1024 rec 16 lap_us 225 now_us 5 clock timemgr"]
    t = start
    for i in range(n):
        rl, t = round_lines(t, (gaps or [20000])[i % len(gaps or [20000])])
        lines += rl
        t += 2000                                         # the application before its next send
    lines.append("RESULT PASS")
    return "\r".join(lines) + "\r"                         # MoofDiag writes CR lines


def run(text, *args):
    with tempfile.NamedTemporaryFile("w", suffix=".out", delete=False, newline="") as fh:
        fh.write(text)
        name = fh.name
    try:
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = te.main(list(args) + [name])
        return rc, buf.getvalue()
    finally:
        os.unlink(name)


class TraceEcho(unittest.TestCase):
    def test_parse_and_labels(self):
        hdr, recs = te.parse(diag_out(1).split("\r"))
        self.assertEqual(hdr["lap_us"], "225")
        self.assertEqual(len(recs), 14)
        for r in recs:
            r.label = te.label(r)
        self.assertEqual(recs[0].label, "PBIN Send")
        self.assertEqual(recs[1].label, "EWRITE PA+1")
        self.assertEqual(recs[2].label, "PBOUT Send(pending)")
        self.assertEqual(recs[7].label, "DATA +1")
        self.assertEqual(recs[8].label, "EWRITE A")
        self.assertFalse(te.keep(recs[13], 17007, False))  # ARP
        self.assertFalse(te.keep(recs[4], 17007, False))   # TICK
        self.assertTrue(te.keep(recs[4], 17007, True))

    def test_medians_per_segment(self):
        rc, out = run(diag_out(5, gaps=[20000, 21000, 22000]), "--quiet")
        self.assertEqual(rc, 0)
        self.assertIn("pattern: 5 of 5 rounds", out)
        # EWDONE -> RX: 0.5 ms to the dropped TICK + 9.0 ms
        self.assertIn("  9.50    9.50    9.50  EWDONE PA+1 -> RX PA+1", out)
        self.assertIn(" 21.00   20.00   22.00  EWRITE A -> PBIN Rcv", out)
        self.assertIn("(next PBIN Send: the application)", out)
        self.assertIn("all rounds, send -> next send: median", out)

    def test_per_round_listing_and_wrap(self):
        rc, out = run(diag_out(3, start=(1 << 32) - 30000))  # the clock wraps inside the first round
        self.assertEqual(rc, 0)
        self.assertIn("round 1: 12 events", out)
        self.assertIn("   +9.50  RX PA+1", out)
        self.assertNotIn("-4294", out)

    def test_subtract_lap(self):
        rc, out = run(diag_out(2), "--quiet", "--subtract-lap")
        line = [ln for ln in out.split("\n") if ln.endswith("EWDONE PA+1 -> RX PA+1")][0]
        self.assertAlmostEqual(float(line.split()[0]), 9.275, delta=0.006)

    def test_no_records(self):
        rc, out = run("MoofDiag at tick 1\rRESULT PASS\r")
        self.assertEqual(rc, 1)
        self.assertIn("no TR records", out)


if __name__ == "__main__":
    unittest.main()
