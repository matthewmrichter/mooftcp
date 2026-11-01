#!/usr/bin/env bash
# Round-trip test of the .sit writer (tools/sit/mksit, Plan 3 Task 11): builds mksit with gcc, makes
# an archive of three synthetic MacBinary files (both forks / data only / resource only; odd sizes,
# an empty fork, a long name with a space, Finder flags) and reads it back with unar and lsar
# (independent of the writer): names, type/creator, Finder flags, fork lengths and sha256 must match.
# Also checks that a non-MacBinary input and a missing input make mksit exit non-zero without
# leaving an archive. Exit 0 only if everything passes. Part of scripts/check-all.sh.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
for t in gcc unar lsar python3; do command -v "$t" >/dev/null || { echo "test-sit.sh: $t not installed" >&2; exit 1; }; done
out="$ROOT/build/sit"
mkdir -p "$out"
gcc -std=c99 -Wall -Wextra -Werror -O2 -I"$ROOT/tools/sit" -o "$out/mksit" \
  "$ROOT/tools/sit/mksit.c" "$ROOT/tools/sit/classic_sit_writer.c" "$ROOT/tools/sit/stream.c" "$ROOT/tools/sit/sit_crc.c"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
python3 -I -B - "$ROOT" "$out/mksit" "$tmp" <<'PY'
import hashlib, importlib.util, json, os, struct, subprocess, sys
root, mksit, tmp = sys.argv[1:4]
spec = importlib.util.spec_from_file_location("relverify", os.path.join(root, "scripts", "relverify.py"))
rv = importlib.util.module_from_spec(spec); spec.loader.exec_module(rv)

def crc16(b):
    c = 0
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c

def mb(path, name, typ, cre, flags, data, rsrc):
    h = bytearray(128)
    nb = name.encode("mac_roman")
    h[1] = len(nb); h[2:2 + len(nb)] = nb
    h[65:69] = typ.encode(); h[69:73] = cre.encode()
    h[73] = flags >> 8; h[101] = flags & 255
    h[83:91] = struct.pack(">II", len(data), len(rsrc))
    h[91:95] = struct.pack(">I", 3000000000); h[95:99] = struct.pack(">I", 3000000001)
    h[122] = 129; h[123] = 129
    h[124:126] = struct.pack(">H", crc16(h[:124]))
    pad = lambda b: b + b"\0" * (-len(b) % 128)
    open(path, "wb").write(bytes(h) + pad(data) + pad(rsrc))
    return rv.rec(name, typ, cre, flags, data, rsrc)

rnd = bytes((i * 7 + (i >> 8)) & 255 for i in range(100003))
want, paths = [], []
for i, (name, typ, cre, fl, d, r) in enumerate([
        ("Both Forks", "APPL", "tst1", 0x2000, rnd[:12345], rnd[1000:78901]),
        ("Data Only, name with spaces 123", "TEXT", "ttxt", 0x0000, b"line\rline\r" * 33, b""),
        ("Rsrc", "cdev", "ztcp", 0x2001, b"", rnd[:129])]):
    p = os.path.join(tmp, "in%d.bin" % i)
    want.append(mb(p, name, typ, cre, fl, d, r)); paths.append(p)
sit = os.path.join(tmp, "t.sit")
subprocess.run([mksit, sit] + paths, check=True)
got = rv.read_unar(sit)
print(rv.compare("sit round trip", got, want))
# error paths
bad = os.path.join(tmp, "bad.bin"); open(bad, "wb").write(b"not macbinary" * 20)
for args in ([bad], [os.path.join(tmp, "missing.bin")]):
    o = os.path.join(tmp, "e.sit")
    r = subprocess.run([mksit, o] + args, stderr=subprocess.DEVNULL)
    if r.returncode == 0 or os.path.exists(o):
        sys.exit("mksit accepted bad input %r" % args)
r = subprocess.run([mksit, os.path.join(tmp, "x.sit")], stderr=subprocess.DEVNULL)
if r.returncode == 0:
    sys.exit("mksit without inputs exited 0")
print("mksit rejects bad, missing and no input")
PY
echo "RESULT PASS test-sit"
