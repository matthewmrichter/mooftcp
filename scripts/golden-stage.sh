#!/usr/bin/env bash
# Stage 2 of docs/dev/GOLDEN-IMAGE.md: copy the System Startup floppy's System Folder onto
# the HD SC Setup-initialized WIP disk, bless it, install boot blocks, add MacTCP and an
# empty "MoofTCP Test" folder. Run only while Snow is stopped.
#
#   scripts/golden-stage.sh [wip-image]     default: $MT_HOME/disks/golden-wip.hda
#
#   MT_MACTCP=2.1   (default) BlueSCSI-distributed MacTCP "2.1" (patched 2.0.6; lists
#                   "Ethernet Built-In" with the DaynaPORT SCSI/Link driver)
#   MT_MACTCP=2.0.6 Apple MacTCP 2.0.6 from media/mactcp206.dsk (no Ethernet on the Classic)
#
# Never targets golden.hda: the golden image is produced only by scripts/golden-promote.sh.
# All refusals happen before anything (dd, hmount) touches the target.
set -euo pipefail
MT_HOME=${MT_HOME:-$HOME/.local/share/mooftcp}
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HDA=${1:-$MT_HOME/disks/golden-wip.hda}
MT_MACTCP=${MT_MACTCP:-2.1}
MEDIA="$MT_HOME/media"

die() { echo "FATAL: $*" >&2; exit 1; }

# ---- guards (read-only) -------------------------------------------------------------
pgrep -x snowemu >/dev/null && die "snowemu is running; stop it first"
[ -f "$HDA" ] || die "missing $HDA (run Stage 1 first)"
[ "$(basename -- "$HDA")" != "golden.hda" ] || die "refusing to stage into golden.hda; use the WIP image"
if [ -f "$ROOT/tools/golden.sha256" ]; then
  [ "$(sha256sum "$HDA" | cut -d' ' -f1)" != "$(tr -d '[:space:]' < "$ROOT/tools/golden.sha256")" ] \
    || die "$HDA is the recorded golden image (hash matches tools/golden.sha256); refusing"
fi
(cd "$MEDIA" && grep -v '^#' "$ROOT/tools/owner-media.sha256" | sha256sum --quiet -c -) \
  || die "owner media in $MEDIA do not match tools/owner-media.sha256"
if [ "$MT_MACTCP" = 2.1 ]; then
  (cd "$MEDIA/daynaport753" && grep -v '^#' "$ROOT/tools/daynaport753.sha256" | grep -F 'MacTCP-2.1.bin' \
    | sha256sum --quiet -c -) || die "MacTCP-2.1.bin missing or bad; run scripts/fetch-daynaport753.sh"
fi

T=$(mktemp -d)
trap 'humount "$T/probe.hda" >/dev/null 2>&1 || true; humount "$T/sys.raw" >/dev/null 2>&1 || true;
      humount "$T/mactcp.dsk" >/dev/null 2>&1 || true; humount "$HDA" >/dev/null 2>&1 || true; rm -rf "$T"' EXIT

# Must be a freshly initialized volume: inspect a throwaway copy (hmount writes the header).
cp -- "$HDA" "$T/probe.hda"
hmount "$T/probe.hda" >/dev/null
if hls -1 ":" | grep -qx "System Folder"; then die "$HDA already contains a System Folder; start again from Stage 1"; fi
humount "$T/probe.hda"
rm -f "$T/probe.hda"

# ---- gather inputs --------------------------------------------------------------------
# Raw HFS volume from the DiskCopy 4.2 floppy (strip 84-byte header, keep 1440K).
dd if="$MEDIA/sys608.img" of="$T/sys.raw" bs=84 skip=1 status=none
truncate -s 1474560 "$T/sys.raw"
cp "$MEDIA/mactcp206.dsk" "$T/mactcp.dsk"

# 1. Pull every System Folder file out as MacBinary (keeps both forks, type, creator).
mkdir "$T/sf"
hmount "$T/sys.raw" >/dev/null
hls -1 ":System Folder:" > "$T/list"
while IFS= read -r f; do hcopy -m ":System Folder:$f" "$T/sf/$f.bin"; done < "$T/list"
humount "$T/sys.raw"
# 2. MacTCP cdev.
case "$MT_MACTCP" in
  2.1) cp "$MEDIA/daynaport753/MacTCP-2.1.bin" "$T/MacTCP.bin" ;;
  2.0.6) hmount "$T/mactcp.dsk" >/dev/null; hcopy -m ":MacTCP" "$T/MacTCP.bin"; humount "$T/mactcp.dsk" ;;
  *) die "MT_MACTCP must be 2.1 or 2.0.6" ;;
esac

# 3. Boot blocks: HD SC Setup leaves them zeroed. Copy the floppy's (LK v0x17,
#    System/Finder, startup app Finder) into the first 1024 bytes of the HFS partition.
off=$(python3 -I - "$HDA" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read(512 * 64)
n = struct.unpack('>I', d[516:520])[0]
for i in range(n):
    e = d[512 * (1 + i):512 * (2 + i)]
    if e[48:80].split(b'\0')[0] == b'Apple_HFS':
        print(struct.unpack('>I', e[8:12])[0]); break
PY
)
[ -n "$off" ] || die "no Apple_HFS partition in $HDA"
dd if="$T/sys.raw" of="$HDA" bs=512 count=2 seek="$off" conv=notrunc status=none

# 4. Write into the HDD's HFS partition.
hmount "$HDA" >/dev/null
hmkdir ":System Folder"
while IFS= read -r f; do hcopy -m "$T/sf/$f.bin" ":System Folder:"; done < "$T/list"
hcopy -m "$T/MacTCP.bin" ":System Folder:"
hattrib -b ":System Folder"
hmkdir ":MoofTCP Test"
hls -la ":System Folder"
humount "$HDA"
echo "staged: $HDA (HFS partition at block $off)"
