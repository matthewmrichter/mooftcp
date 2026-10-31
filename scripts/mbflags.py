#!/usr/bin/env python3
"""Set the Finder flags of a MacBinary II file and fix its header CRC.

    scripts/mbflags.py <file.bin> <flags>      e.g. 0x2000 (hasBundle)

Retro68's Rez writes MacBinary with Finder flags 0; the MoofTCP control panel needs hasBundle
(0x2000) so the Finder reads its BNDL and shows its icon (the real MacTCP file has 0x2000 once
installed, MACTCP-INTERFACE.md "File and resource layout"). The high byte goes to header offset
73, the low byte to 101 (MacBinary II), and the CRC-16/XMODEM of bytes 0..123 to 124..125.
"""
import sys


def crc16(data):
    c = 0
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    path, flags = sys.argv[1], int(sys.argv[2], 0)
    d = bytearray(open(path, "rb").read())
    if len(d) < 128 or d[0] != 0 or d[74] != 0:
        sys.exit("mbflags.py: %s is not a MacBinary file" % path)
    d[73] = (flags >> 8) & 0xFF
    d[101] = flags & 0xFF
    c = crc16(bytes(d[:124]))
    d[124], d[125] = c >> 8, c & 0xFF
    open(path, "wb").write(bytes(d))


if __name__ == "__main__":
    main()
