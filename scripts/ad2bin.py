#!/usr/bin/env python3
"""Make a MacBinary II file from what `unar -k visible` writes: the data fork <name> (may be
missing) plus the AppleDouble resource/Finder-info file <name>.rsrc.
    ad2bin.py <unar-output-path-without-.rsrc> <out.bin> [mac-name]
    ad2bin.py --rewrap <in.bin> <out.bin>     (re-encode a MacBinary file as clean MacBinary II)
Used for the test apps in $MT_HOME/media/apps (docs/dev/APPS.md); the .bin goes to deploy.sh."""
import os
import struct
import sys


def crc16_xmodem(b):
    c = 0
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def appledouble(path):
    d = open(path, "rb").read()
    magic, _ver = struct.unpack(">II", d[:8])
    if magic not in (0x00051607, 0x00051600):
        sys.exit("ad2bin.py: %s is not AppleDouble/AppleSingle" % path)
    n = struct.unpack(">H", d[24:26])[0]
    ent = {}
    for i in range(n):
        eid, off, ln = struct.unpack(">III", d[26 + 12 * i:38 + 12 * i])
        ent[eid] = d[off:off + ln]
    return ent


def main(a):
    if len(a) == 3 and a[0] == "--rewrap":
        rewrap(a[1], a[2])
        return
    if len(a) < 2:
        sys.exit("usage: ad2bin.py <path-without-.rsrc> <out.bin> [mac-name]")
    base, out = a[0], a[1]
    name = (a[2] if len(a) > 2 else os.path.basename(base)).encode("mac_roman")[:63]
    data = open(base, "rb").read() if os.path.isfile(base) else b""
    ent = appledouble(base + ".rsrc") if os.path.isfile(base + ".rsrc") else {}
    rsrc = ent.get(2, b"")
    finfo = (ent.get(9, b"") + b"\0" * 32)[:32]
    ftype, creator, flags = finfo[0:4], finfo[4:8], struct.unpack(">H", finfo[8:10])[0]
    if ftype == b"\0\0\0\0":
        sys.exit("ad2bin.py: no Finder info for %s" % base)
    # Dates from the AppleDouble "file dates" entry (seconds since 2000, signed) so the output is
    # reproducible; Mac dates count from 1904.
    if len(ent.get(8, b"")) >= 8:
        cr, md = struct.unpack(">ii", ent[8][:8])
        tc, t = (cr + 3029529600) & 0xFFFFFFFF, (md + 3029529600) & 0xFFFFFFFF
    else:   # unar restores the archive's modification time on the files it writes
        f = base if os.path.isfile(base) else base + ".rsrc"
        tc = t = (int(os.path.getmtime(f)) + 2082844800) & 0xFFFFFFFF
    write(out, name, ftype, creator, flags, tc, t, data, rsrc)


def write(out, name, ftype, creator, flags, tc, t, data, rsrc):
    h = bytearray(128)
    h[1] = len(name)
    h[2:2 + len(name)] = name
    h[65:69] = ftype
    h[69:73] = creator
    h[73] = (flags >> 8) & 0xFF & ~0x01  # clear "on desk"
    struct.pack_into(">II", h, 83, len(data), len(rsrc))
    struct.pack_into(">II", h, 91, tc, t)
    h[101] = flags & 0xFF
    h[122] = 129
    h[123] = 129
    struct.pack_into(">H", h, 124, crc16_xmodem(bytes(h[:124])))

    def pad(b):
        return b + b"\0" * ((-len(b)) % 128)
    with open(out, "wb") as f:
        f.write(bytes(h) + pad(data) + pad(rsrc))
    print("%s: %s/%s data %d rsrc %d" % (out, ftype.decode("mac_roman"), creator.decode("mac_roman"),
                                       len(data), len(rsrc)))


def rewrap(src, out):
    """A MacBinary file hfsutils rejects (hexbin's output for some BinHex files) -> clean MacBinary II."""
    b = open(src, "rb").read()
    n = b[1]
    dl, rl = struct.unpack(">II", b[83:91])
    tc, t = struct.unpack(">II", b[91:99])
    flags = (b[73] << 8) | b[101]
    data = b[128:128 + dl]
    roff = 128 + dl + ((-dl) % 128)
    write(out, b[2:2 + n], b[65:69], b[69:73], flags, tc, t, data, b[roff:roff + rl])


if __name__ == "__main__":
    main(sys.argv[1:])
