#!/usr/bin/env bash
# Put MoofTCP instead of MacTCP on $MT_HOME/disks/work.hda (Snow must be stopped).
#   install-moof.sh [--no-prefs | --prefs <MoofTCP Prefs.bin>] [--cdev <MoofTCP.bin>] [--dnr] [--keep-mactcp]
# 1. Moves the real "MacTCP", "MacTCP DNR" and "MacTCP Prep" from :System Folder: into
#    ":MacTCP Parked:" (hrename keeps forks and type/creator; EMULATOR.md "Method" step 3), and removes
#    MoofTCP files of an earlier install (MoofTCP, MoofTCP Prefs, the interim "MacTCP" if already parked).
# 2. Copies the MoofTCP control panel (default build-mac/moof/MoofTCP.bin, `scripts/build-mac.sh MoofTCP`)
#    and the prefs file (default: built by scripts/mkprefs.sh for Snow's NAT: 10.0.0.2/8,
#    gateway 10.0.0.1, DNS 1.1.1.1) into :System Folder:. --no-prefs leaves MoofTCP unconfigured.
#    --dnr also installs the test-only resolver file "MacTCP DNR" (build-mac/moof/MacTCP DNR.bin; the
#    release does not ship it: apps find dnrp 0 in the MoofTCP file, Plan 3 Task 8).
#    --keep-mactcp leaves the real MacTCP trio where it is (MoofTCP next to MacTCP: the INIT back-off test).
# Only work.hda is touched (deploy.sh refuses golden.hda and a running Snow).
set -euo pipefail
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="$MT_HOME/disks/work.hda"
die() { echo "install-moof.sh: $*" >&2; exit 1; }

prefs="default"; cdev="$ROOT/build-mac/moof/MoofTCP.bin"; dnr=""; keepmac=""
while [ $# -gt 0 ]; do
  case "$1" in
    --no-prefs) prefs=""; shift;;
    --prefs) [ $# -ge 2 ] || die "--prefs needs a file"; prefs="$2"; shift 2;;
    --cdev) [ $# -ge 2 ] || die "--cdev needs a file"; cdev="$2"; shift 2;;
    --dnr) dnr=1; shift;;
    --keep-mactcp) keepmac=1; shift;;
    *) die "usage: install-moof.sh [--no-prefs | --prefs <file.bin>] [--cdev <MoofTCP.bin>] [--dnr] [--keep-mactcp]";;
  esac
done
[ -f "$cdev" ] || die "no $cdev (run scripts/build-mac.sh MoofTCP)"
dnrfile=""
[ -z "$dnr" ] || [ -z "$keepmac" ] || die "--dnr and --keep-mactcp exclude each other (MacTCP's own MacTCP DNR stays)"
if [ -n "$dnr" ]; then
  dnrfile="$(dirname "$cdev")/MacTCP DNR.bin"
  [ -f "$dnrfile" ] || die "no MacTCP DNR.bin next to $cdev (run scripts/build-mac.sh MoofTCP)"
fi
if [ "$prefs" = default ]; then
  "$ROOT/scripts/mkprefs.sh" --ip 10.0.0.2 --mask 255.0.0.0 --gw 10.0.0.1 --dns 1.1.1.1 >/dev/null
  prefs="$ROOT/build-mac/prefs/MoofTCP Prefs.bin"
fi
if "$ROOT/scripts/snow.sh" status >/dev/null 2>&1 || pgrep -x snowemu >/dev/null; then
  die "Snow is running; stop it first (scripts/snow.sh stop)"
fi

part="$(python3 -I -c '
import sys
d = open(sys.argv[1], "rb").read(512 * 64)
n = 0
for i in range(1, 64):
    b = d[512*i:512*i+512]
    if b[:2] != b"PM":
        break
    if b[48:80].split(b"\0")[0] == b"Apple_HFS":
        n += 1
        print(n)
        break
' "$IMG")"
[ -n "$part" ] || die "no Apple_HFS partition in $IMG"
vol=""
cleanup() { if [ -n "$vol" ]; then humount "$vol:" >/dev/null 2>&1 || true; fi; }
trap cleanup EXIT
vol="$(hmount "$IMG" "$part" | sed -n 's/^Volume name is "\(.*\)"$/\1/p')"
[ -n "$vol" ] || die "hmount failed"
# exists <folder:> <name>: hls exits 0 even for a missing path, so look the name up in the listing
exists() { hls -1a "$1" 2>/dev/null | grep -Fxq -- "$2"; }
exists ":" "MacTCP Parked" || hmkdir ":MacTCP Parked:"
for f in "MacTCP" "MacTCP DNR" "MacTCP Prep" "MoofTCP" "MoofTCP Prefs"; do
  case "$f" in MoofTCP*) ;; *) [ -z "$keepmac" ] || continue;; esac   # --keep-mactcp: leave MacTCP's
  if exists ":System Folder:" "$f"; then
    if exists ":MacTCP Parked:" "$f" || [ "${f#MoofTCP}" != "$f" ]; then
      hdel ":System Folder:$f"            # already parked: this one is MoofTCP's from an earlier install
      echo "removed :System Folder:$f"
    else
      hrename ":System Folder:$f" ":MacTCP Parked:$f"
      echo "parked :System Folder:$f"
    fi
  fi
done
humount >/dev/null 2>&1 || true; vol=""
"$ROOT/scripts/deploy.sh" "$cdev" ${prefs:+"$prefs"} ${dnrfile:+"$dnrfile"} --to ":System Folder:" | grep -E "copied|MacTCP|MoofTCP" || true
