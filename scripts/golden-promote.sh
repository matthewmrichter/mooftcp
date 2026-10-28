#!/usr/bin/env bash
# Final step of docs/dev/GOLDEN-IMAGE.md: after the guest was shut down from inside
# (Special > Shut Down) and Snow was stopped, promote the WIP image to golden.hda and
# record its hash in tools/golden.sha256. Never overwrites an existing golden.hda.
#
#   scripts/golden-promote.sh [wip-image]   default: $MT_HOME/disks/golden-wip.hda
set -euo pipefail
MT_HOME=${MT_HOME:-$HOME/.local/share/mooftcp}
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WIP=${1:-$MT_HOME/disks/golden-wip.hda}
GOLDEN="$MT_HOME/disks/golden.hda"

die() { echo "FATAL: $*" >&2; exit 1; }
pgrep -x snowemu >/dev/null && die "snowemu is running; shut the guest down and stop Snow first"
[ -f "$WIP" ] || die "missing $WIP"
[ ! -e "$GOLDEN" ] || die "$GOLDEN already exists; move it aside deliberately if you are rebuilding"

hash="$(sha256sum "$WIP" | cut -d' ' -f1)"
mv -n -- "$WIP" "$GOLDEN"
[ ! -e "$WIP" ] || die "move failed"
[ "$(sha256sum "$GOLDEN" | cut -d' ' -f1)" = "$hash" ] || die "hash changed during promote"
echo "$hash" > "$ROOT/tools/golden.sha256"
echo "promoted: $GOLDEN ($hash) -> tools/golden.sha256"
