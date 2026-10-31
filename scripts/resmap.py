#!/usr/bin/env python3
"""Print the resource map of a MacBinary (.bin) file: type, id, name, size, attributes.

    scripts/resmap.py <file.bin> [--hex TYPE ID [N]]

The same columns as the MacTCP table in docs/dev/MACTCP-INTERFACE.md ("File and resource
layout"): attributes are the resource-map byte (0x40 sysHeap, 0x20 purgeable, 0x10 locked,
0x08 protected, 0x04 preload, 0x02 changed). --hex dumps the first N bytes (default 64) of one
resource. Reads only the file given; no Mac tools needed.
"""
import struct
import sys


def macbinary(path):
    d = open(path, "rb").read()
    if len(d) < 128 or d[0] != 0 or d[74] != 0:
        sys.exit("resmap.py: %s is not a MacBinary file" % path)
    name = d[2:2 + d[1]].decode("mac_roman")
    ftype, creator = d[65:69].decode("mac_roman"), d[69:73].decode("mac_roman")
    dlen, rlen = struct.unpack(">II", d[83:91])
    flags = (d[73] << 8) | d[101]
    off = 128 + ((dlen + 127) // 128) * 128
    return name, ftype, creator, flags, dlen, d[off:off + rlen]


def resources(r):
    if not r:
        return []
    doff, moff, dlen, mlen = struct.unpack(">IIII", r[:16])
    m = r[moff:moff + mlen]
    tl_off, nl_off = struct.unpack(">HH", m[24:28])
    tl = m[tl_off:]
    ntypes = struct.unpack(">H", tl[:2])[0] + 1
    out = []
    for i in range(ntypes):
        t, n, rl = struct.unpack(">4sHH", tl[2 + 8 * i:10 + 8 * i])
        for k in range(n + 1):
            e = tl[rl + 12 * k:rl + 12 * k + 12]
            rid, noff = struct.unpack(">hH", e[:4])
            attr = e[4]
            dof = (e[5] << 16) | (e[6] << 8) | e[7]
            size = struct.unpack(">I", r[doff + dof:doff + dof + 4])[0]
            data = r[doff + dof + 4:doff + dof + 4 + size]
            nm = ""
            if noff != 0xFFFF:
                p = nl_off + noff
                nm = m[p + 1:p + 1 + m[p]].decode("mac_roman")
            out.append((t.decode("mac_roman"), rid, nm, size, attr, data))
    return out


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    name, ftype, creator, flags, dlen, r = macbinary(a[0])
    res = resources(r)
    if len(a) >= 4 and a[1] == "--hex":
        n = int(a[4]) if len(a) > 4 else 64
        for t, rid, nm, size, attr, data in res:
            if t == a[2] and rid == int(a[3]):
                for o in range(0, min(n, size), 16):
                    print("%04x: %s" % (o, " ".join("%02x" % b for b in data[o:o + 16])))
                return
        sys.exit("resmap.py: no resource %s %s" % (a[2], a[3]))
    print("%s: type %s creator %s finder flags 0x%04x data fork %d resource fork %d" %
          (name, ftype, creator, flags, dlen, len(r)))
    print("%-6s %6s  %-12s %8s  %s" % ("Type", "ID", "Name", "Bytes", "Attr"))
    for t, rid, nm, size, attr, _ in res:
        print("%-6s %6d  %-12s %8d  0x%02x" % ("'%s'" % t, rid, nm, size, attr))


if __name__ == "__main__":
    main()
