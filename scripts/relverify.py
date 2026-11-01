#!/usr/bin/env python3
"""Independent verification of the release artifacts (helper of scripts/release-verify.sh).

    relverify.py [--self-test] [--dist DIR] <version>

Reads dist/MoofTCP-<version>.{hqx,sit,dsk} with readers that did not write them:
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
The .hqx holds the installer only; the .sit and the .dsk hold both files, and the .dsk must hold
nothing else at its root and have the volume name "MoofTCP" and 819200 bytes.

--self-test first requires the real artifacts to PASS, then corrupts a copy of each (a resource-fork
byte in each format, plus the type code in the .dsk) and requires the verification of every copy to
FAIL. Exit 0 only if all of that holds. Without --self-test: exit 0 only if every format passes.
"""
import hashlib
import json
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


def expected():
    if not os.path.isfile(INSTALLER_BIN):
        raise Fail("missing reference " + INSTALLER_BIN)
    inst = parse_macbinary(INSTALLER_BIN)
    if inst["name"] != "MoofTCP Installer" or inst["type"] != "APPL" or inst["creator"] != "mtin":
        raise Fail("reference installer has unexpected identity %r" % inst)
    if not inst["flags"] & 0x2000:
        raise Fail("reference installer lacks the bundle bit")
    txt = open(README_SRC, "rb").read()
    if txt.endswith(b"\n") is False:
        txt += b"\n"
    txt = txt.replace(b"\r\n", b"\n").replace(b"\n", b"\r")
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
        o = out.setdefault(nm, dict(type=None, creator=None, flags=0))
        if "XADFileType" in e:
            o["type"], o["creator"] = cc(e["XADFileType"]), cc(e["XADFileCreator"])
            o["flags"] |= e.get("XADFinderFlags", 0)
    tmp = tempfile.mkdtemp(prefix="relv-unar-")
    try:
        run(["unar", "-q", "-D", "-k", "visible", "-o", tmp, archive])
        res = []
        for nm, o in out.items():
            p = os.path.join(tmp, nm)
            data = open(p, "rb").read() if os.path.isfile(p) else b""
            rp = p + ".rsrc"
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
        names = [n for n in run(["hls", "-1", ":"]).decode("mac_roman").split("\n") if n]
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
        for name, cpaths, label in cases:
            res = [r for r in verify_all(cpaths, inst, readme) if r[0] == label]
            if res[0][1]:
                print("SELF-TEST FAIL: corrupted %s still passes (%s)" % (name, label))
                good = False
            else:
                print("SELF-TEST ok: corrupted %s fails (%s): %s" % (name, label, res[0][2][:110]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return good


def main():
    args = sys.argv[1:]
    st = False
    dist = os.path.join(ROOT, "dist")
    while args and args[0].startswith("--"):
        a = args.pop(0)
        if a == "--self-test":
            st = True
        elif a == "--dist" and args:
            dist = args.pop(0)
        else:
            sys.exit(__doc__)
    if len(args) != 1:
        sys.exit(__doc__)
    ver = args[0]
    paths = {k: os.path.join(dist, "MoofTCP-%s.%s" % (ver, k)) for k in ("hqx", "sit", "dsk")}
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
