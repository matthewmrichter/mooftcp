#!/usr/bin/env python3
"""Release version check (scripts/release.sh): relvers.py <version> <MoofTCP Installer.bin>

The short version string of 'vers' 1 must equal <version> in the installer app AND in the MoofTCP
file it embeds (resource 'MTCP' 128 = that file's resource fork, byte for byte). Exit 1 otherwise."""
import importlib.util
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("resmap", os.path.join(here, "resmap.py"))
resmap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(resmap)


def vers1(rsrc, what):
    for t, rid, _nm, _size, _attr, data in resmap.resources(rsrc):
        if t == "vers" and rid == 1:
            n = data[6]                       # packed vers: 4 bytes, region word, then the short Pascal string
            return data[7:7 + n].decode("mac_roman")
    sys.exit("relvers.py: no 'vers' 1 in %s" % what)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    ver, path = sys.argv[1:]
    r = resmap.macbinary(path)[5]
    emb = None
    for t, rid, _nm, _size, _attr, data in resmap.resources(r):
        if t == "MTCP" and rid == 128:
            emb = data
    if emb is None:
        sys.exit("relvers.py: no 'MTCP' 128 in the installer")
    bad = [(w, v) for w, v in (("installer app", vers1(r, "the installer")),
                               ("embedded MoofTCP file", vers1(emb, "the embedded MoofTCP file"))) if v != ver]
    for w, v in bad:
        print("relvers.py: %s has version %r, release is %r" % (w, v, ver), file=sys.stderr)
    sys.exit(1 if bad else 0)


main()
