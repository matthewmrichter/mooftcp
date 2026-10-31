#!/usr/bin/env bash
# Put MoofTCP instead of MacTCP on $MT_HOME/disks/work.hda (Snow must be stopped).
#   install-moof.sh [--no-prefs | --prefs <MoofTCP Prefs.bin>] [--cdev <MacTCP.bin>]
# 1. Moves the real "MacTCP", "MacTCP DNR" and "MacTCP Prep" from :System Folder: into
#    ":MacTCP Parked:" (hrename keeps forks and type/creator; EMULATOR.md "Method" step 3).
# 2. Copies the MoofTCP cdev (default build-mac/moof/MacTCP.bin, `scripts/build-mac.sh MoofTCP`),
#    the resolver file "MacTCP DNR" (build-mac/moof/MacTCP DNR.bin, next to the cdev) and the prefs file (default: built by scripts/mkprefs.sh for Snow's NAT: 10.0.0.2/8,
#    gateway 10.0.0.1, DNS 1.1.1.1) into :System Folder:. --no-prefs leaves MoofTCP unconfigured.
# Only work.hda is touched (deploy.sh refuses golden.hda and a running Snow).
set -euo pipefail
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="$MT_HOME/disks/work.hda"
die() { echo "install-moof.sh: $*" >&2; exit 1; }

prefs="default"; cdev="$ROOT/build-mac/moof/MacTCP.bin"
while [ $# -gt 0 ]; do
  case "$1" in
    --no-prefs) prefs=""; shift;;
    --prefs) [ $# -ge 2 ] || die "--prefs needs a file"; prefs="$2"; shift 2;;
    --cdev) [ $# -ge 2 ] || die "--cdev needs a file"; cdev="$2"; shift 2;;
    *) die "usage: install-moof.sh [--no-prefs | --prefs <file.bin>] [--cdev <MacTCP.bin>]";;
  esac
done
[ -f "$cdev" ] || die "no $cdev (run scripts/build-mac.sh MoofTCP)"
[ -f "$(dirname "$cdev")/MacTCP DNR.bin" ] || die "no MacTCP DNR.bin next to $cdev (run scripts/build-mac.sh MoofTCP)"
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
for f in "MacTCP" "MacTCP DNR" "MacTCP Prep" "MoofTCP Prefs"; do
  if exists ":System Folder:" "$f"; then
    if exists ":MacTCP Parked:" "$f" || [ "$f" = "MoofTCP Prefs" ]; then
      hdel ":System Folder:$f"            # already parked: this one is MoofTCP's from an earlier install
      echo "removed :System Folder:$f"
    else
      hrename ":System Folder:$f" ":MacTCP Parked:$f"
      echo "parked :System Folder:$f"
    fi
  fi
done
humount >/dev/null 2>&1 || true; vol=""
dnrfile="$(dirname "$cdev")/MacTCP DNR.bin"      # the resolver file apps look for (Task 11)
"$ROOT/scripts/deploy.sh" "$cdev" ${prefs:+"$prefs"} ${dnrfile:+"$dnrfile"} --to ":System Folder:" | grep -E "copied|MacTCP|MoofTCP" || true
