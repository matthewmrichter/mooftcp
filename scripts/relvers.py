#!/usr/bin/env python3
"""Release version check (scripts/release.sh): relvers.py [--lite] <version> <MoofTCP Installer.bin>

The short version string of 'vers' 1 must equal <version> in the installer app AND in the MoofTCP
file it embeds (resource 'MTCP' 128 = that file's resource fork, byte for byte). The variant is
checked on the long version strings of both: with --lite (MoofTCP Lite, docs/dev/RELEASE.md
"Variants") each must contain "MoofTCP Lite", without it neither may contain "Lite". Exit 1 otherwise."""
import importlib.util
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("resmap", os.path.join(here, "resmap.py"))
resmap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(resmap)


def vers1(rsrc, what):
    """(short, long) version strings of 'vers' 1."""
    for t, rid, _nm, _size, _attr, data in resmap.resources(rsrc):
        if t == "vers" and rid == 1:
            n = data[6]                       # packed vers: 4 bytes, region word, then the short Pascal string
            m = data[7 + n]                   # then the long Pascal string
            return (data[7:7 + n].decode("mac_roman"), data[8 + n:8 + n + m].decode("mac_roman"))
    sys.exit("relvers.py: no 'vers' 1 in %s" % what)


def main():
    args = sys.argv[1:]
    lite = False
    if args and args[0] == "--lite":
        lite = True
        args = args[1:]
    if len(args) != 2:
        sys.exit(__doc__)
    ver, path = args
    r = resmap.macbinary(path)[5]
    emb = None
    for t, rid, _nm, _size, _attr, data in resmap.resources(r):
        if t == "MTCP" and rid == 128:
            emb = data
    if emb is None:
        sys.exit("relvers.py: no 'MTCP' 128 in the installer")
    bad = []
    for w, (short, long_) in (("installer app", vers1(r, "the installer")),
                              ("embedded MoofTCP file", vers1(emb, "the embedded MoofTCP file"))):
        if short != ver:
            bad.append("%s has version %r, release is %r" % (w, short, ver))
        if lite and "MoofTCP Lite" not in long_:
            bad.append("%s is not MoofTCP Lite (long version %r), --lite was given" % (w, long_))
        if not lite and "Lite" in long_:
            bad.append("%s is MoofTCP Lite (long version %r), the full release was asked for" % (w, long_))
    for b in bad:
        print("relvers.py: " + b, file=sys.stderr)
    sys.exit(1 if bad else 0)


main()
