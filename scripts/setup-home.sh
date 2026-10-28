#!/usr/bin/env bash
# Create $MT_HOME tree and copy owner-provided ROMs/media into it (never overwrites differing files).
set -euo pipefail
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
SRC_ROMS="${MT_SRC_ROMS:-$HOME/projects/Old_World_Mac_Roms}"
SRC_IMGS="${MT_SRC_IMGS:-$HOME/projects/Mac Images}"
mkdir -p "$MT_HOME"/{snow,rom,disks,media}

safe_copy() { # src dst
  local src="$1" dst="$2"
  [ -f "$src" ] || { echo "FATAL: missing source: $src" >&2; exit 1; }
  if [ -e "$dst" ]; then
    if cmp -s "$src" "$dst"; then echo "ok (unchanged): $dst"; return; fi
    echo "FATAL: $dst exists and differs from $src; refusing to overwrite" >&2; exit 1
  fi
  cp -- "$src" "$dst"; echo "copied: $dst"
}

safe_copy "$SRC_ROMS/512KB ROMs/1990-10 - A49F9914 - Mac Classic.rom" "$MT_HOME/rom/MacClassic.rom"
safe_copy "$SRC_ROMS/256KB ROMs/1987-03 - B2E362A8 - Mac SE.ROM" "$MT_HOME/rom/MacSE.rom"
safe_copy "$SRC_IMGS/MacOS_6.0.8_System_Startup.img" "$MT_HOME/media/sys608.img"
safe_copy "$SRC_IMGS/MacTCP-2-0.6.dsk" "$MT_HOME/media/mactcp206.dsk"

# DaynaPORT: unar -k visible writes data forks plus AppleDouble ".rsrc" resource forks.
dp="$MT_HOME/media/daynaport"
if [ -n "$(ls -A "$dp" 2>/dev/null)" ]; then
  echo "ok (already unpacked): $dp"
else
  mkdir -p "$dp"
  unar -q -k visible -o "$dp" "$SRC_IMGS/DaynaPORT_DCv4_Stuffitv1.5.sit"
  echo "unpacked: $dp"
fi
