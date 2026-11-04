#!/usr/bin/env python3
"""Release version check (scripts/release.sh): relvers.py <version> <MoofTCP Installer.bin>

The short version string of 'vers' 1 must equal <version> in the installer app AND in both MoofTCP
files it embeds (resources 'MTCP' 128 = MoofTCP, 'MTCP' 129 = MoofTCP Lite, each that file's resource
fork byte for byte; docs/dev/RELEASE.md "Variants"). The variant is checked on the long version
strings: 128 must not contain "Lite", 129 must start with "MoofTCP Lite"; the installer's own must not
say Lite (there is one installer for both). Exit 1 otherwise."""
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
    if len(args) != 2:
        sys.exit(__doc__)
    ver, path = args
    r = resmap.macbinary(path)[5]
    emb = {}
    for t, rid, _nm, _size, _attr, data in resmap.resources(r):
        if t == "MTCP" and rid in (128, 129):
            emb[rid] = data
    bad = []
    for rid in (128, 129):
        if rid not in emb:
            bad.append("no 'MTCP' %d in the installer" % rid)
    checks = [("installer app", vers1(r, "the installer"), False)]
    checks += [("embedded MoofTCP file 'MTCP' %d" % rid, vers1(emb[rid], "'MTCP' %d" % rid), rid == 129)
               for rid in (128, 129) if rid in emb]
    for w, (short, long_), lite in checks:
        if short != ver:
            bad.append("%s has version %r, release is %r" % (w, short, ver))
        if lite and not long_.startswith("MoofTCP Lite"):
            bad.append("%s must be MoofTCP Lite (long version %r)" % (w, long_))
        if not lite and "Lite" in long_:
            bad.append("%s must not be Lite (long version %r)" % (w, long_))
    for b in bad:
        print("relvers.py: " + b, file=sys.stderr)
    sys.exit(1 if bad else 0)


main()
