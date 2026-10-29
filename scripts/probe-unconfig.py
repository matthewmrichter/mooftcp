#!/usr/bin/env python3
"""Leave MacTCP without an IP address on $MT_HOME/disks/work.hda (Snow must be stopped).

    scripts/probe-unconfig.py [<image>] [--mode zero-ip|delete|factory]

zero-ip (default): rewrite the resource fork of ":System Folder:MacTCP Prep" so that the
    'ipln' 128 address field (the IP address the golden image carries, 10.0.0.2 at offset 8
    of the resource) becomes 0.0.0.0. Everything else (mask, router, DNS) is untouched.
    The file is copied out and back as MacBinary (hcopy -m), so type, creator and forks survive.
delete: remove "MacTCP Prep" only (not enough: the configured address is also kept in the
    MacTCP control panel file itself, so MacTCP still comes up with 10.0.0.2).
factory: remove "MacTCP Prep" and "MacTCP DNR" and reinstall the pristine MacTCP 2.1 file from
    $MT_HOME/media/daynaport753/MacTCP-2.1.bin (factory configuration: no address). This is
    what probe-run.sh --unconfigured uses, together with the Ethernet link Down so that no
    RARP answer can supply an address either.

Only the image passed (default work.hda) is touched; golden.hda is refused.
"""
import os
import subprocess
import sys
import tempfile

MT_HOME = os.environ.get("MT_HOME", os.path.expanduser("~/.local/share/mooftcp"))
PREP = ":System Folder:MacTCP Prep"


def die(msg):
    print("probe-unconfig.py: " + msg, file=sys.stderr)
    sys.exit(1)


def sh(*cmd, check=True):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if check and r.returncode:
        die("%s failed: %s" % (" ".join(cmd), r.stderr.strip()))
    return r


def hfs_partition(img):
    d = open(img, "rb").read(512 * 64)
    n = 0
    for i in range(1, 64):
        b = d[512 * i:512 * i + 512]
        if b[:2] != b"PM":
            break
        if b[48:80].split(b"\0")[0] == b"Apple_HFS":
            n += 1
            return n
    die("no Apple_HFS partition in " + img)


def main():
    args = sys.argv[1:]
    mode = "zero-ip"
    if "--mode" in args:
        i = args.index("--mode")
        mode = args[i + 1]
        del args[i:i + 2]
    img = args[0] if args else os.path.join(MT_HOME, "disks", "work.hda")
    if os.path.realpath(img) == os.path.realpath(os.path.join(MT_HOME, "disks", "golden.hda")):
        die("refusing golden.hda")
    if mode not in ("zero-ip", "delete", "factory"):
        die("mode must be zero-ip, delete or factory")
    if subprocess.run(["pgrep", "-x", "snowemu"], capture_output=True).returncode == 0:
        die("Snow is running; stop it first")
    out = sh("hmount", img, str(hfs_partition(img)))
    try:
        if mode == "factory":
            pristine = os.path.join(MT_HOME, "media", "daynaport753", "MacTCP-2.1.bin")
            if not os.path.isfile(pristine):
                die("missing " + pristine)
            for f in (PREP, ":System Folder:MacTCP DNR", ":System Folder:MacTCP"):
                sh("hdel", f, check=False)
            sh("hcopy", "-m", pristine, ":System Folder:")
            print("factory MacTCP 2.1 installed; Prep and DNR removed")
            print(sh("hls", "-l", ":System Folder:").stdout, end="")
            return
        if mode == "delete":
            sh("hdel", PREP)
            print("deleted " + PREP)
            return
        with tempfile.TemporaryDirectory() as td:
            mb = os.path.join(td, "prep.bin")
            sh("hcopy", "-m", PREP, mb)
            data = bytearray(open(mb, "rb").read())
            # MacBinary: 128-byte header (its CRC covers only the header, so editing fork
            # bytes needs no CRC change), data fork (empty here), then the resource fork.
            # 'ipln' 128 holds: 00000001 00000800 <ip> <mask> ... in the golden image.
            key = bytes.fromhex("0000000100000800")
            pos = data.find(key, 128)
            if pos < 0 or data.find(key, pos + 1) >= 0:
                die("did not find exactly one ipln header in the Prep resource fork")
            old = bytes(data[pos + 8:pos + 12])
            data[pos + 8:pos + 12] = b"\0\0\0\0"
            open(mb, "wb").write(data)
            sh("hdel", PREP)
            sh("hcopy", "-m", mb, ":System Folder:")
            print("MacTCP Prep: ipln address %s -> 0.0.0.0" % ".".join(str(b) for b in old))
        print(sh("hls", "-l", ":System Folder:").stdout, end="")
    finally:
        subprocess.run(["humount"], capture_output=True)


if __name__ == "__main__":
    main()
