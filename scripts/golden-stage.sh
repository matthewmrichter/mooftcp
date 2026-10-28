#!/bin/bash
# Stage 2 of docs/dev/GOLDEN-IMAGE.md: copy the System Startup floppy's System Folder onto
# the HD SC Setup-initialized HDD, bless it, install boot blocks, add MacTCP and an empty
# "MoofTCP Test" folder. Run only while Snow is stopped.
#   MT_MACTCP=2.1   (default) BlueSCSI-distributed MacTCP "2.1" (patched 2.0.6; lists
#                   "Ethernet Built-In" with the DaynaPORT SCSI/Link driver)
#   MT_MACTCP=2.0.6 Apple MacTCP 2.0.6 from media/mactcp206.dsk (no Ethernet on the Classic)
set -euo pipefail
MT_HOME=${MT_HOME:-$HOME/.local/share/mooftcp}
HDA=${1:-$MT_HOME/disks/golden.hda}
MT_MACTCP=${MT_MACTCP:-2.1}
if pgrep -x snowemu >/dev/null; then echo "FATAL: snowemu is running" >&2; exit 1; fi
T=$(mktemp -d)
trap 'humount >/dev/null 2>&1 || true; rm -rf "$T"' EXIT

# Raw HFS volume from the DiskCopy 4.2 floppy (strip 84-byte header, keep 1440K).
dd if="$MT_HOME/media/sys608.img" of="$T/sys.raw" bs=84 skip=1 status=none
truncate -s 1474560 "$T/sys.raw"
cp "$MT_HOME/media/mactcp206.dsk" "$T/mactcp.dsk"

# 1. Pull every System Folder file out as MacBinary (keeps both forks, type, creator).
mkdir "$T/sf"
hmount "$T/sys.raw" >/dev/null
hls -1 ":System Folder:" > "$T/list"
while IFS= read -r f; do hcopy -m ":System Folder:$f" "$T/sf/$f.bin"; done < "$T/list"
humount
# 2. MacTCP cdev.
case "$MT_MACTCP" in
  2.1) cp "$MT_HOME/media/daynaport753/MacTCP-2.1.bin" "$T/MacTCP.bin" ;;  # verified by fetch-daynaport753.sh
  2.0.6) hmount "$T/mactcp.dsk" >/dev/null; hcopy -m ":MacTCP" "$T/MacTCP.bin"; humount ;;
  *) echo "FATAL: MT_MACTCP must be 2.1 or 2.0.6" >&2; exit 1 ;;
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
[ -n "$off" ] || { echo "no Apple_HFS partition in $HDA" >&2; exit 1; }
dd if="$T/sys.raw" of="$HDA" bs=512 count=2 seek="$off" conv=notrunc status=none

# 4. Write into the HDD's HFS partition.
hmount "$HDA" >/dev/null
hmkdir ":System Folder"
while IFS= read -r f; do hcopy -m "$T/sf/$f.bin" ":System Folder:"; done < "$T/list"
hcopy -m "$T/MacTCP.bin" ":System Folder:"
hattrib -b ":System Folder"
hmkdir ":MoofTCP Test"
hls -la ":System Folder"
humount
echo "staged: $HDA (HFS partition at block $off)"
