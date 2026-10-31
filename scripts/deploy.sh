#!/usr/bin/env bash
# Copy MacBinary (.bin) files onto $MT_HOME/disks/work.hda.
#   deploy.sh <file.bin>... --to "<HFS:path:>"     e.g. --to ":MoofTCP Test:"
# Refuses while Snow runs. Only ever touches work.hda (never golden.hda).
# The HFS partition number for hmount is found by reading the Apple partition map
# (hfsutils numbers only Apple_HFS partitions, 1-based, in map order).
# If the destination folder is missing it is created with hmkdir (single level).
set -euo pipefail
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="$MT_HOME/disks/work.hda"
GOLDEN="$MT_HOME/disks/golden.hda"
die() { echo "deploy.sh: $*" >&2; exit 1; }

files=(); dest=""
while [ $# -gt 0 ]; do
  case "$1" in
    --to) [ $# -ge 2 ] || die "--to needs a destination"; dest="$2"; shift 2;;
    -*) die "unknown option: $1";;
    *) files+=("$1"); shift;;
  esac
done
{ [ ${#files[@]} -gt 0 ] && [ -n "$dest" ]; } || die 'usage: deploy.sh <file.bin>... --to ":Folder:"'

if "$ROOT/scripts/snow.sh" status >/dev/null 2>&1 || pgrep -x snowemu >/dev/null; then
  die "Snow is running; stop it first (scripts/snow.sh stop)"
fi
[ -f "$IMG" ] || die "missing $IMG (run scripts/fresh-disk.sh)"
if [ -e "$GOLDEN" ] && [ "$(readlink -f "$IMG")" = "$(readlink -f "$GOLDEN")" ]; then
  die "target resolves to golden.hda; refusing"
fi
for f in "${files[@]}"; do [ -f "$f" ] || die "no such file: $f"; done

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
[ -n "$part" ] || die "no Apple_HFS partition found in $IMG"

vol=""
cleanup() { if [ -n "$vol" ]; then humount "$vol:" >/dev/null 2>&1 || true; fi; }
trap cleanup EXIT
vol="$(hmount "$IMG" "$part" | sed -n 's/^Volume name is "\(.*\)"$/\1/p')"
[ -n "$vol" ] || die "hmount failed on $IMG partition $part"
case "$dest" in :*) path="$vol$dest";; *) path="$dest";; esac
case "$path" in *:) ;; *) path="$path:";; esac
# hls exits 0 even for a missing path, so look the folder name up in its parent's listing.
p="${path%:}"; pname="${p##*:}"; parent="${p%:*}:"
if [ "$parent" != "$path" ] && ! hls -1a "$parent" 2>/dev/null | grep -Fxq -- "$pname"; then
  hmkdir "$path" || die "destination $path missing and hmkdir failed"
  echo "created folder $path"
fi
for f in "${files[@]}"; do
  hcopy -m -- "$f" "$path"
  echo "copied $f -> $path"
done
hls -l "$path"
