#!/usr/bin/env bash
# Verify the golden System 6.0.8 image against tools/golden.sha256, then copy it to
# $MT_HOME/disks/work.hda. Every emulator test boots work.hda; golden.hda is never booted.
set -euo pipefail
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GOLDEN="$MT_HOME/disks/golden.hda"
WORK="$MT_HOME/disks/work.hda"
HASHFILE="$ROOT/tools/golden.sha256"

die() { echo "FATAL: $*" >&2; exit 1; }

[ -f "$HASHFILE" ] || die "missing $HASHFILE (golden image not recorded yet; see docs/dev/GOLDEN-IMAGE.md)"
[ -f "$GOLDEN" ] || die "missing $GOLDEN (build it per docs/dev/GOLDEN-IMAGE.md)"
want="$(tr -d '[:space:]' < "$HASHFILE")"
[[ "$want" =~ ^[0-9a-f]{64}$ ]] || die "$HASHFILE does not hold a SHA-256 hex digest"
if pgrep -x snowemu >/dev/null; then
  die "snowemu is running; stop it before copying disk images"
fi

have="$(sha256sum "$GOLDEN" | cut -d' ' -f1)"
[ "$have" = "$want" ] || die "golden image hash mismatch: $GOLDEN is $have, expected $want"

tmp="$WORK.tmp.$$"
trap 'rm -f "$tmp"' EXIT
cp -- "$GOLDEN" "$tmp"
copy="$(sha256sum "$tmp" | cut -d' ' -f1)"
[ "$copy" = "$want" ] || die "copy hash mismatch ($copy)"
mv -f -- "$tmp" "$WORK"
trap - EXIT
echo "fresh: $WORK ($want)"
