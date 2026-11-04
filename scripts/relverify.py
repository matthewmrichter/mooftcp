#!/usr/bin/env python3
"""Independent verification of the release artifacts (helper of scripts/release-verify.sh).

    relverify.py [--self-test] [--lite] [--dist DIR] <version>

Reads dist/MoofTCP-<version>.{hqx,sit,dsk} with readers that did not write them (--lite: MoofTCP Lite,
dist/MoofTCP-Lite-<version>.{hqx,sit,dsk} against build-mac-lite/, volume name "MoofTCP Lite", application
"MoofTCP Lite Installer"):
  .hqx  macutils `hexbin` (-> MacBinary) and `unar`/`lsar` (The Unarchiver)
  .sit  `unar`/`lsar`
  .dsk  hfsutils `hmount`/`hls`/`hcopy -m`
and compares every file found with what it must be:
  "MoofTCP Installer": build-mac/installer/MoofTCP Installer.bin (name, type, creator, Finder flags,
                       fork lengths, sha256 of the data and resource forks);
  "Read Me":           derived here from docs/release/ReadMe.txt (LF -> CR, type ttro, creator ttxt,
                       flags 0, no resource fork), not taken from release.sh's output.
Compared Finder flags: all 16 bits except 0x0100 (kHasBeenInited: the Finder sets it by itself when
it first shows a file, so a copy that was looked at may differ). 0x2000 (hasBundle) is compared.
Dates are not compared (HFS, SIT and BinHex store them differently, and hfsutils stamps its own).
The .dsk root is listed with `hls -a` (invisible files such as a stray Desktop file count as extra files).
hfsck (hfsutils' volume checker) is not installed on this host and is not used; the structure check is
hmount (MDB) plus a successful hcopy of every file.
The .hqx holds the installer only; the .sit and the .dsk hold both files, and the .dsk must hold
nothing else at its root and have the volume name "MoofTCP" and 819200 bytes.

--self-test first requires the real artifacts to PASS, then corrupts a copy of each (a resource-fork
byte in each format, plus the type code in the .dsk) and requires the verification of every copy to
FAIL. Exit 0 only if all of that holds. Without --self-test: exit 0 only if every format passes.
"""
import hashlib
import json
import re
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FLAG_MASK = 0xFEFF
INSTALLER_BIN = os.path.join(ROOT, "build-mac", "installer", "MoofTCP Installer.bin")
README_SRC = os.path.join(ROOT, "docs", "release", "ReadMe.txt")
VOLNAME = "MoofTCP"
INST_NAME = "MoofTCP Installer"       # --lite: "MoofTCP Lite Installer"


class Fail(Exception):
    pass


def sha(b):
    return hashlib.sha256(b).hexdigest()


def rec(name, typ, creator, flags, data, rsrc):
    return dict(name=name, type=typ, creator=creator, flags=flags & FLAG_MASK,
                dlen=len(data), rlen=len(rsrc), dsha=sha(data), rsha=sha(rsrc))


def parse_macbinary(path):
    d = open(path, "rb").read()
    if len(d) < 128 or d[0] != 0 or d[74] != 0 or not 1 <= d[1] <= 63:
        raise Fail("%s: not MacBinary" % path)
    dl, rl = struct.unpack(">II", d[83:91])
    roff = 128 + ((dl + 127) // 128) * 128
    if roff + rl > len(d) and rl:
        raise Fail("%s: truncated MacBinary" % path)
    return rec(d[2:2 + d[1]].decode("mac_roman"), d[65:69].decode("latin-1"), d[69:73].decode("latin-1"),
               (d[73] << 8) | d[101], d[128:128 + dl], d[roff:roff + rl])



def apply_diaglog(txt, on):
    """The one rule for the diagnostic-log block (switch: scripts/release.conf DIAGLOG_TEXT)."""
    if on:
        return re.sub(rb"(?m)^\[(BEGIN|END) DIAGNOSTIC LOG TEXT[^\n]*\n", b"", txt)
    return re.sub(rb"(?ms)^\[BEGIN DIAGNOSTIC LOG TEXT[^\n]*\n.*?^\[END DIAGNOSTIC LOG TEXT[^\n]*\n(\n){0,2}", b"", txt)


def diaglog_switch():
    m = re.search(r"(?m)^DIAGLOG_TEXT=([01])\s*$", open(os.path.join(ROOT, "scripts", "release.conf")).read())
    if not m:
        raise Fail("scripts/release.conf has no DIAGLOG_TEXT=0|1 line")
    return m.group(1) == "1"


def expected(switch=None):
    if switch is None:
        switch = diaglog_switch()
    if not os.path.isfile(INSTALLER_BIN):
        raise Fail("missing reference " + INSTALLER_BIN)
    inst = parse_macbinary(INSTALLER_BIN)
    if inst["name"] != INST_NAME or inst["type"] != "APPL" or inst["creator"] != "mtin":
        raise Fail("reference installer has unexpected identity %r" % inst)
    if not inst["flags"] & 0x2000:
        raise Fail("reference installer lacks the bundle bit")
    txt = open(README_SRC, "rb").read()
    txt = apply_diaglog(txt, switch)
    if txt.endswith(b"\n") is False:
        txt += b"\n"
    txt = txt.replace(b"\r\n", b"\n").replace(b"\n", b"\r")
    low = txt.lower()
    if (b"diagnostic log" in low or b"keep a" in low) != switch:
        raise Fail("Read Me text and DIAGLOG_TEXT=%d disagree" % switch)
    readme = rec("Read Me", "ttro", "ttxt", 0, txt, b"")
    return inst, readme


def run(cmd, **kw):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw)
    if r.returncode != 0:
        raise Fail("%s failed (%d): %s" % (cmd[0], r.returncode, r.stderr.decode("latin-1", "replace").strip()[:300]))
    return r.stdout


def cc(v):
    return struct.pack(">I", v).decode("latin-1")


def appledouble_rsrc(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x00\x05\x16\x07":
        raise Fail("%s: not AppleDouble" % path)
    n = struct.unpack(">H", d[24:26])[0]
    for i in range(n):
        eid, off, ln = struct.unpack(">III", d[26 + 12 * i:38 + 12 * i])
        if eid == 2:
            return d[off:off + ln]
    return b""


def read_unar(archive):
    """Entries as seen by unar + lsar."""
    meta = json.loads(run(["lsar", "-j", archive]), strict=False)["lsarContents"]
    out = {}
    for e in meta:
        nm = e["XADFileName"]
        o = out.setdefault(nm, dict(type=None, creator=None, flags=0, need_data=False, need_rsrc=False))
        if e.get("XADFileSize", 0) > 0:
            o["need_rsrc" if e.get("XADIsResourceFork") else "need_data"] = True
        if "XADFileType" in e:
            o["type"], o["creator"] = cc(e["XADFileType"]), cc(e["XADFileCreator"])
            o["flags"] |= e.get("XADFinderFlags", 0)
    tmp = tempfile.mkdtemp(prefix="relv-unar-")
    try:
        run(["unar", "-q", "-D", "-k", "visible", "-o", tmp, archive])
        res = []
        for nm, o in out.items():
            p = os.path.join(tmp, nm)
            rp = p + ".rsrc"
            if o["need_data"] and not os.path.isfile(p):
                raise Fail("unar listed a data fork for %r but extracted no file" % nm)
            if o["need_rsrc"] and not os.path.isfile(rp):
                raise Fail("unar listed a resource fork for %r but extracted no .rsrc file" % nm)
            data = open(p, "rb").read() if os.path.isfile(p) else b""
            rsrc = appledouble_rsrc(rp) if os.path.isfile(rp) else b""
            res.append(rec(nm, o["type"], o["creator"], o["flags"], data, rsrc))
        extra = [f for f in os.listdir(tmp) if f not in out and not (f.endswith(".rsrc") and f[:-5] in out)]
        if extra:
            raise Fail("unar produced unexpected files: %r" % extra)
        return res
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def read_hexbin(hqx):
    tmp = tempfile.mkdtemp(prefix="relv-hexbin-")
    try:
        run(["hexbin", os.path.abspath(hqx)], cwd=tmp)
        files = os.listdir(tmp)
        if len(files) != 1:
            raise Fail("hexbin produced %r" % files)
        return [parse_macbinary(os.path.join(tmp, files[0]))]
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def read_dsk(dsk):
    if os.path.getsize(dsk) != 819200:
        raise Fail("%s is %d bytes, not 819200" % (dsk, os.path.getsize(dsk)))
    tmp = tempfile.mkdtemp(prefix="relv-hfs-")
    mounted = False
    try:
        out = run(["hmount", os.path.abspath(dsk)]).decode("mac_roman")
        mounted = True
        if 'Volume name is "%s"' % VOLNAME not in out:
            raise Fail("volume name is not %r: %s" % (VOLNAME, out.splitlines()[:1]))
        names = [n for n in run(["hls", "-a", "-1", ":"]).decode("mac_roman").split("\n") if n]
        res = []
        for i, nm in enumerate(names):
            p = os.path.join(tmp, "f%d.bin" % i)
            run(["hcopy", "-m", ":" + nm, p])
            r = parse_macbinary(p)
            if r["name"] != nm:
                raise Fail("hcopy name %r != listing %r" % (r["name"], nm))
            res.append(r)
        return res
    finally:
        if mounted:
            subprocess.run(["humount"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        shutil.rmtree(tmp, ignore_errors=True)


def compare(label, got, want):
    errs = []
    gotd = {}
    for g in got:
        if g["name"] in gotd:
            errs.append("duplicate entry %r" % g["name"])
        gotd[g["name"]] = g
    for w in want:
        g = gotd.pop(w["name"], None)
        if g is None:
            errs.append("missing %r" % w["name"])
            continue
        for k in ("type", "creator", "flags", "dlen", "rlen", "dsha", "rsha"):
            if g[k] != w[k]:
                errs.append("%r %s: got %r, want %r" % (w["name"], k, g[k], w[k]))
    for n in gotd:
        errs.append("unexpected entry %r" % n)
    if errs:
        raise Fail(label + ": " + "; ".join(errs))
    return "; ".join("%s type/creator %s/%s flags %04x data %d rsrc %d" %
                     (g["name"], g["type"], g["creator"], g["flags"], g["dlen"], g["rlen"]) for g in got)


def verify_all(paths, inst, readme):
    """paths: dict hqx/sit/dsk -> file. Returns list of (label, ok, text)."""
    results = []
    jobs = [("hqx via hexbin", paths["hqx"], read_hexbin, [inst]),
            ("hqx via unar", paths["hqx"], read_unar, [inst]),
            ("sit via unar", paths["sit"], read_unar, [inst, readme]),
            ("dsk via hfsutils", paths["dsk"], read_dsk, [inst, readme])]
    for label, p, reader, want in jobs:
        try:
            if not os.path.isfile(p):
                raise Fail("missing " + p)
            results.append((label, True, compare(label, reader(p), want)))
        except Fail as e:
            results.append((label, False, str(e)))
        except Exception as e:  # a reader crash is a failure, never a pass
            results.append((label, False, "%s: %s" % (type(e).__name__, e)))
    return results


def report(results):
    ok = True
    for label, good, text in results:
        print("%s %s: %s" % ("PASS" if good else "FAIL", label, text))
        ok = ok and good
    return ok


def flip(path, offset):
    with open(path, "r+b") as f:
        f.seek(offset)
        b = f.read(1)
        f.seek(offset)
        f.write(bytes([b[0] ^ 0x01]))


def find_unique(path, needle):
    d = open(path, "rb").read()
    i = d.find(needle)
    if i < 0 or d.find(needle, i + 1) >= 0:
        raise Fail("self-test: cannot place a unique marker in " + path)
    return i


def crc16x(b):
    c = 0
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def fix_crc(h):
    h[124:126] = struct.pack(">H", crc16x(bytes(h[:124])))


def readme_macbinary(path, switch):
    txt = open(README_SRC, "rb").read()
    txt = apply_diaglog(txt, switch)
    if not txt.endswith(b"\n"):
        txt += b"\n"
    data = txt.replace(b"\r\n", b"\n").replace(b"\n", b"\r")
    h = bytearray(128)
    h[1] = 7
    h[2:9] = b"Read Me"
    h[65:69] = b"ttro"
    h[69:73] = b"ttxt"
    h[83:87] = struct.pack(">I", len(data))
    h[122] = h[123] = 129
    fix_crc(h)
    open(path, "wb").write(bytes(h) + data + b"\0" * (-len(data) % 128))
    return data


def mksit_path():
    exe = os.path.join(ROOT, "build", "sit", "mksit")
    if not os.path.isfile(exe):
        os.makedirs(os.path.dirname(exe), exist_ok=True)
        src = os.path.join(ROOT, "tools", "sit")
        run(["gcc", "-std=c99", "-O2", "-I" + src, "-o", exe] +
            [os.path.join(src, f) for f in ("mksit.c", "classic_sit_writer.c", "stream.c", "sit_crc.c")])
    return exe


def binhex_file(binfile, out):
    body = run(["binhex", binfile]).decode("latin-1").split("\n")
    if body and body[0].startswith("("):
        body = body[1:]
    open(out, "w", newline="").write("(This file must be converted with BinHex 4.0)\n" + "\n".join(body))


def wrong_archive_cases(tmp, paths):
    """CRC-valid wrong artifacts: the readers accept them, only the comparison can reject them."""
    ref = bytearray(open(INSTALLER_BIN, "rb").read())
    dl, rl = struct.unpack(">II", bytes(ref[83:91]))
    roff = 128 + ((dl + 127) // 128) * 128
    mk = mksit_path()
    rm = os.path.join(tmp, "Read Me.bin")
    rdata = readme_macbinary(rm, diaglog_switch())
    cases = []

    def inst_variant(name, mutate):
        b = bytearray(ref)
        b = mutate(b)
        f = os.path.join(tmp, name)
        open(f, "wb").write(bytes(b))
        return f

    def creator(b):
        b[69:73] = b"XXXX"
        fix_crc(b)
        return b

    def swap_rsrc(b):
        data = bytes(b[128:128 + dl]) + b"\0" * (-dl % 128) if dl else b""
        h = bytearray(b[:128])
        h[87:91] = struct.pack(">I", len(rdata))
        fix_crc(h)
        return bytearray(bytes(h) + data + rdata + b"\0" * (-len(rdata) % 128))

    def flags(b):
        b[73] = 0
        fix_crc(b)
        return b

    for nm, mut, lbl in (("creator", creator, "sit creator XXXX"), ("swap", swap_rsrc, "sit resource fork swapped for Read Me bytes"),
                         ("flags", flags, "sit bundle bit cleared")):
        f = inst_variant("inst-%s.bin" % nm, mut)
        sit = os.path.join(tmp, "w-%s.sit" % nm)
        run([mk, sit, f, rm])
        cases.append((lbl, dict(paths, sit=sit), "sit via unar"))
    sit = os.path.join(tmp, "w-drop.sit")
    run([mk, sit, INSTALLER_BIN])
    cases.append(("sit without the Read Me entry", dict(paths, sit=sit), "sit via unar"))
    f = inst_variant("inst-hq.bin", creator)
    hq = os.path.join(tmp, "w-creator.hqx")
    binhex_file(f, hq)
    cases.append(("hqx of a creator-XXXX file", dict(paths, hqx=hq), "hqx via hexbin"))
    cases.append(("hqx of a creator-XXXX file", dict(paths, hqx=hq), "hqx via unar"))
    trunc = os.path.join(tmp, "trunc.dsk")
    shutil.copy(paths["dsk"], trunc)
    os.truncate(trunc, 819199)
    cases.append(("dsk truncated to 819199 bytes", dict(paths, dsk=trunc), "dsk via hfsutils"))
    # dsk with an extra invisible file
    inv = os.path.join(tmp, "inv.dsk")
    shutil.copy(paths["dsk"], inv)
    run(["hmount", inv])
    try:
        run(["hcopy", "-m", rm, ":Desktop"])
        run(["hattrib", "+i", ":Desktop"])
    finally:
        subprocess.run(["humount"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    cases.append(("dsk with an extra invisible file", dict(paths, dsk=inv), "dsk via hfsutils"))
    # a Read Me made with the OPPOSITE diagnostic-log switch (valid archive, wrong text)
    rm2 = os.path.join(tmp, "Read Me.bin")
    readme_macbinary(rm2, not diaglog_switch())
    sit = os.path.join(tmp, "w-diag.sit")
    run([mk, sit, INSTALLER_BIN, rm2])
    cases.append(("sit whose Read Me disagrees with DIAGLOG_TEXT", dict(paths, sit=sit), "sit via unar"))
    return cases


def switch_tests():
    """The DIAGLOG_TEXT rule itself: OFF removes the whole block and says nothing about the log, ON keeps it."""
    src = open(README_SRC, "rb").read()
    off, on = apply_diaglog(src, False), apply_diaglog(src, True)
    good = True
    for name, cond in (
            ("OFF text has no 'diagnostic log' / 'Keep a' and no marker", b"diagnostic log" not in off.lower() and b"keep a" not in off.lower() and b"DIAGNOSTIC LOG TEXT" not in off),
            ("OFF text has no empty 'REPORTING PROBLEMS' heading", b"REPORTING PROBLEMS" not in off),
            ("ON text has the block and no marker", b"Keep a diagnostic log" in on and b"DIAGNOSTIC LOG TEXT" not in on),
            ("OFF is shorter than ON", len(off) < len(on))):
        print("SELF-TEST %s: %s" % ("ok" if cond else "FAIL", name))
        good = good and cond
    # expected() holds either way: ON needs the block in the source, OFF needs it gone
    for sw in (False, True):
        try:
            expected(sw)
            print("SELF-TEST ok: expected(%s) is consistent with ReadMe.txt" % sw)
        except Fail as e:
            print("SELF-TEST FAIL: expected(%s): %s" % (sw, e))
            good = False
    return good


def self_test(paths, inst, readme):
    ok = report(verify_all(paths, inst, readme))
    if not ok:
        print("SELF-TEST ABORT: the pristine artifacts do not pass")
        return False
    rsrc = open(INSTALLER_BIN, "rb").read()
    dl = struct.unpack(">I", rsrc[83:87])[0]
    roff = 128 + ((dl + 127) // 128) * 128
    rl = struct.unpack(">I", rsrc[87:91])[0]
    marker = rsrc[roff + rl // 2:roff + rl // 2 + 48]
    tmp = tempfile.mkdtemp(prefix="relv-self-")
    good = True
    try:
        cases = []
        # sit: stored, so the marker is in the file verbatim
        c = dict(paths, sit=os.path.join(tmp, "bad.sit"))
        shutil.copy(paths["sit"], c["sit"])
        flip(c["sit"], find_unique(c["sit"], marker) + 7)
        cases.append(("sit resource-fork byte", c, "sit via unar"))
        # dsk: stored contiguously
        c = dict(paths, dsk=os.path.join(tmp, "bad.dsk"))
        shutil.copy(paths["dsk"], c["dsk"])
        flip(c["dsk"], find_unique(c["dsk"], marker) + 7)
        cases.append(("dsk resource-fork byte", c, "dsk via hfsutils"))
        # dsk: wrong type code via hattrib
        c = dict(paths, dsk=os.path.join(tmp, "badtype.dsk"))
        shutil.copy(paths["dsk"], c["dsk"])
        run(["hmount", c["dsk"]])
        try:
            run(["hattrib", "-t", "XXXX", ":Read Me"])
        finally:
            subprocess.run(["humount"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cases.append(("dsk type code", c, "dsk via hfsutils"))
        # hqx: change one body character to another valid BinHex character
        c = dict(paths, hqx=os.path.join(tmp, "bad.hqx"))
        d = bytearray(open(paths["hqx"], "rb").read())
        i = len(d) // 2
        while d[i] in b"\r\n:":
            i += 1
        d[i] = ord("!") if d[i] != ord("!") else ord('"')
        open(c["hqx"], "wb").write(d)
        cases.append(("hqx body character", c, "hqx via hexbin"))
        cases.append(("hqx body character", c, "hqx via unar"))
        cases += wrong_archive_cases(tmp, paths)
        for name, cpaths, label in cases:
            res = [r for r in verify_all(cpaths, inst, readme) if r[0] == label]
            if res[0][1]:
                print("SELF-TEST FAIL: corrupted %s still passes (%s)" % (name, label))
                good = False
            else:
                print("SELF-TEST ok: corrupted %s fails (%s): %s" % (name, label, res[0][2][:110]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return good and switch_tests()


def main():
    global INSTALLER_BIN, VOLNAME, INST_NAME
    args = sys.argv[1:]
    st = False
    stem = "MoofTCP-"
    dist = os.path.join(ROOT, "dist")
    while args and args[0].startswith("--"):
        a = args.pop(0)
        if a == "--self-test":
            st = True
        elif a == "--lite":
            INSTALLER_BIN = os.path.join(ROOT, "build-mac-lite", "installer", "MoofTCP Installer.bin")
            VOLNAME = "MoofTCP Lite"
            INST_NAME = "MoofTCP Lite Installer"
            stem = "MoofTCP-Lite-"
        elif a == "--dist" and args:
            dist = args.pop(0)
        else:
            sys.exit(__doc__)
    if len(args) != 1:
        sys.exit(__doc__)
    ver = args[0]
    paths = {k: os.path.join(dist, "%s%s.%s" % (stem, ver, k)) for k in ("hqx", "sit", "dsk")}
    for tool in ("hexbin", "unar", "lsar", "hmount", "hls", "hcopy", "hattrib", "humount"):
        if shutil.which(tool) is None:
            sys.exit("relverify.py: %s not installed" % tool)
    try:
        inst, readme = expected()
    except Fail as e:
        sys.exit("relverify.py: " + str(e))
    if st:
        ok = self_test(paths, inst, readme)
    else:
        ok = report(verify_all(paths, inst, readme))
    print("RESULT " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
