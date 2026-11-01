#!/usr/bin/env python3
"""Select Finder or MultiFinder at startup on work.hda (the Finder's Special > Set Startup does the same).

    mf-startup.py <work.hda> multifinder|finder|show

System 6 keeps the choice in the boot blocks of the startup volume: bbShellName (Str15 at offset 0x5A of the
HFS partition's first block). Set Startup in Snow (Plan 3 Task 12) changed exactly these 16 bytes from
"\\x06Finder" + 9 spaces to "\\x0bMultiFinder" + 81 00 00 42; this script writes the same bytes. Refuses
golden.hda; the caller must have stopped Snow (scripts/mf-run.sh does)."""
import os
import struct
import sys

FINDER = b"\x06Finder" + b" " * 9
MULTI = b"\x0bMultiFinder" + bytes([0x81, 0x00, 0x00, 0x42])


def hfs_start(img):
    with open(img, "rb") as f:
        d = f.read(512 * 64)
    for i in range(1, 64):
        b = d[512 * i:512 * i + 512]
        if b[:2] != b"PM":
            break
        if b[48:80].split(b"\0")[0] == b"Apple_HFS":
            return struct.unpack(">I", b[8:12])[0] * 512
    sys.exit("mf-startup.py: no Apple_HFS partition")


def main():
    if len(sys.argv) != 3 or sys.argv[2] not in ("multifinder", "finder", "show"):
        sys.exit("usage: mf-startup.py <work.hda> multifinder|finder|show")
    img, mode = sys.argv[1], sys.argv[2]
    if os.path.basename(img) == "golden.hda":
        sys.exit("mf-startup.py: refusing golden.hda")
    off = hfs_start(img) + 0x5A
    with open(img, "r+b" if mode != "show" else "rb") as f:
        f.seek(off - 0x5A)
        if f.read(2) != b"LK":
            sys.exit("mf-startup.py: no boot blocks (signature LK) on the HFS partition")
        f.seek(off)
        cur = f.read(16)
        name = cur[1:1 + cur[0]].decode("mac_roman", "replace")
        if mode == "show":
            print(name)
            return
        f.seek(off)
        f.write(MULTI if mode == "multifinder" else FINDER)
    print(f"startup shell: {name} -> {'MultiFinder' if mode == 'multifinder' else 'Finder'}")


main()
