#!/usr/bin/env python3
"""Host-side logic of scripts/keylatsrv.py (KeyLat): line parsing, stream splitting, baseline, report. ctest runs it."""
import importlib.util
import os
import random
import sys
import unittest

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("keylatsrv", os.path.join(here, "..", "scripts", "keylatsrv.py"))
k = importlib.util.module_from_spec(spec)
spec.loader.exec_module(k)

TICK = 1000.0 / 60.147


class Parse(unittest.TestCase):
    def test_lines(self):
        self.assertEqual(k.parse_line(b"KL1 K 5 1000 1001\n"), ("K", 5, 1000, 1001))
        self.assertEqual(k.parse_line(b"KL1 S 7 2000\r\n"), ("S", 7, 2000, None))
        self.assertIsNone(k.parse_line(b"KL1 K 5 1000"))          # a key line needs "seen"
        self.assertIsNone(k.parse_line(b"KL2 K 5 1 2"))
        self.assertIsNone(k.parse_line(b"garbage"))
        self.assertIsNone(k.parse_line(b"KL1 K x 1 2"))

    def test_linebuf_chunks(self):
        lb = k.LineBuf()
        self.assertEqual(lb.feed(b"KL1 K 1 10"), [])
        self.assertEqual(lb.feed(b" 11\nKL1 S 2 12"), [b"KL1 K 1 10 11"])
        self.assertEqual(lb.feed(b"\nKL1 S 3 13\n"), [b"KL1 S 2 12", b"KL1 S 3 13"])

    def test_linebuf_runaway(self):
        lb = k.LineBuf()
        lb.feed(b"x" * 5000)
        self.assertEqual(lb.buf, b"")

    def test_mac_log(self):
        m = k.parse_mac_log("KeyLat 1 10.0.0.1 17061\rK 3 100 101 101 101 103 0 a\rS 4 105 105 105 106 0\rEND\r")
        self.assertEqual(list(m), [3])
        self.assertEqual(m[3]["done"], 103)
        self.assertEqual(m[3]["ch"], "a")


class Hull(unittest.TestCase):
    def test_lower(self):
        h = k.lower_hull([(0, 5), (1, 1), (2, 4), (3, 0), (4, 6)])
        self.assertEqual(h, [(0, 5), (1, 1), (3, 0), (4, 6)])
        self.assertEqual(k.lower_hull([(1, 3), (1, 2)]), [(1, 2)])

    def test_baseline_interp_extrap(self):
        b = k.Baseline([(0, 100.0), (10, 200.0), (20, 300.0), (5, 190.0)])
        self.assertAlmostEqual(b.at(10), 200.0)
        self.assertAlmostEqual(b.at(15), 250.0)
        self.assertAlmostEqual(b.at(-5), 50.0)
        self.assertAlmostEqual(b.at(25), 350.0)
        self.assertTrue(k.Baseline([(7, 70.0)]).ok())
        self.assertAlmostEqual(k.Baseline([(7, 70.0)]).at(8), 70.0 + TICK)
        self.assertFalse(k.Baseline([]).ok())


def synth(seed, rate_hz, owd_ms=1.5, nkeys=40, true_lat=(2.0, 30.0)):
    """A Mac with tick clock at rate_hz, started at host time 5000 ms; pings at tick edges (+0..0.3 ms),
    keys pressed at random real times, event.when = floor(press tick). Returns (events, true latencies)."""
    r = random.Random(seed)
    tick_ms = 1000.0 / rate_hz
    ev = []
    seq = 0
    for i in range(20):                                  # start burst
        seq += 1
        t = 100 + i * 12
        ev.append(("S", seq, t, None, 5000 + t * tick_ms + r.uniform(0, 0.3) + owd_ms + r.uniform(0, 2.0)))
    truth = []
    now_ms = 5000 + 400 * tick_ms
    for i in range(nkeys):
        now_ms += r.uniform(500, 900)
        press_tick = (now_ms - 5000) / tick_ms
        when = int(press_tick)
        lat = r.uniform(*true_lat)
        seq += 1
        ev.append(("K", seq, when, when + 1, now_ms + lat))
        truth.append((lat, press_tick - when))
    for i in range(12):                                  # closing burst
        seq += 1
        t = int((now_ms - 5000) / tick_ms) + 300 + i * 12
        ev.append(("S", seq, t, None, 5000 + t * tick_ms + r.uniform(0, 0.3) + owd_ms + r.uniform(0, 2.0)))
    return ev, truth


class Analyse(unittest.TestCase):
    def test_estimate_within_bounds(self):
        for rate in (60.147, 60.15, 90.0):                # 90 = a fast emulated clock
            ev, truth = synth(1, rate)
            r = k.analyse(ev, 20.0, 2.0, 1000.0 / rate)
            self.assertEqual(len(r["rows"]), 40)
            tick_ms = 1000.0 / rate
            for row, (lat, frac) in zip(r["rows"], truth):
                # L_est = true + (press - when) - (ping path); within [true - 4, true + one tick]
                self.assertGreater(row["L"], lat - 4.0)
                self.assertLess(row["L"], lat + tick_ms + 0.5)
            self.assertAlmostEqual(r["base"].rate_hz(), rate, delta=rate * 0.01)
            self.assertLess(r["cal_median"], 5.0)

    def test_verdict_classes(self):
        ev, _ = synth(2, 60.147, true_lat=(1.0, 4.0))     # fast keys: nothing can be a sure fail
        r = k.analyse(ev)
        self.assertEqual(r["fail"], 0)
        self.assertEqual(r["pass"] + r["inconclusive"] + r["fail"], 40)
        ev, _ = synth(3, 60.147, true_lat=(60.0, 90.0))   # slow keys: all sure fail
        r = k.analyse(ev)
        self.assertEqual(r["fail"], 40)
        self.assertEqual(r["pass"], 0)

    def test_noisy_calibration_warns(self):
        ev, _ = synth(6, 60.147)
        ev = [(kk, s, x, y, t + (random.Random(s).uniform(0, 300) if kk == "S" else 0)) for kk, s, x, y, t in ev]
        text, r = k.report(ev)
        self.assertGreater(r["cal_median"], 5.0)
        self.assertIn("WARNING: noisy", text)

    def test_no_sync(self):
        r = k.analyse([("K", 1, 5, 6, 100.0)])
        self.assertEqual(r["rows"], [])
        text, _ = k.report([("K", 1, 5, 6, 100.0)])
        self.assertIn("no calibration pings", text)

    def test_report_text_and_csv(self):
        ev, _ = synth(4, 60.147)
        mac = {x[1]: {"when": x[2], "seen": x[3], "call": x[3], "ret": x[3] + 1, "done": x[3] + 2, "res": 0, "ch": "a"}
               for x in ev if x[0] == "K"}
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            csv = os.path.join(d, "o.csv")
            text, _ = k.report(ev, mac, csv=csv)
            self.assertIn("Goal 20 ms", text)
            self.assertIn("age = event seen - event.when: 1:40", text)
            self.assertIn("TCPSend call duration: 1:40", text)
            with open(csv) as fh:
                self.assertEqual(len(fh.read().splitlines()), 41)

    def test_raw_roundtrip(self):
        import tempfile
        ev, _ = synth(5, 60.147)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.host.txt")
            with open(p, "w") as f:
                for kk, s, x, y, t in ev:
                    f.write("%s %d %d %s %.4f\n" % (kk, s, x, "-" if y is None else y, t))
            back = k.read_raw(p)
            self.assertEqual(len(back), len(ev))
            self.assertEqual(back[0][:4], ev[0][:4])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + sys.argv[1:])
