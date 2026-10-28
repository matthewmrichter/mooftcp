#!/usr/bin/env bash
# Fetch the DaynaPORT 7.5.3 installer (and the MacTCP "2.1" cdev) from the BlueSCSI
# project's documented distribution, verify, and unpack into $MT_HOME/media/daynaport753/.
# Idempotent: if the verified outputs exist, does nothing. Source + hashes: tools/daynaport753.sha256.
# Offline copy: if $DP753_LOCAL (default: the owner's "Mac Images/BlueSCSI-DaynaPORT-7.5.3")
# holds the zip, it is copied instead of downloading (hash-checked the same way).
# The download is untrusted data: nothing in it is executed on the host; hfsutils only
# reads files out of the bundled HFS disk image.
set -euo pipefail
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUMS="$ROOT/tools/daynaport753.sha256"
DEST="$MT_HOME/media/daynaport753"
URL="$(sed -n 's/^# url: //p' "$SUMS")"
ZIP="HD1 BlueSCSI Bootstrap.hda.zip"
HDA="HD1 BlueSCSI Bootstrap.hda"
DP="DaynaPORT 7.5.3-DiskCopy4.img"
TCP="MacTCP-2.1.bin"
DP753_LOCAL="${DP753_LOCAL:-${MT_SRC_IMGS:-$HOME/projects/Mac Images}/BlueSCSI-DaynaPORT-7.5.3}"

die() { echo "FATAL: $*" >&2; exit 1; }
check() { (cd "$DEST" && grep -v '^#' "$SUMS" | sha256sum --quiet -c -); }

mkdir -p "$DEST"
if [ -f "$DEST/$DP" ] && [ -f "$DEST/$TCP" ] && [ -f "$DEST/$ZIP" ] && check; then
  echo "ok (verified): $DEST"; exit 0
fi

# Preflight (only when there is work to do). The download itself is Python urllib in
# mega-get.py (no curl); decryption needs python3-cryptography.
need() { command -v "$1" >/dev/null || die "$1 not found; owner must install $2 (see CLAUDE.md Bootstrap)"; }
need python3 python3; need unzip unzip; need hmount hfsutils; need hcopy hfsutils
python3 -I -c 'import cryptography' 2>/dev/null \
  || die "python3 module 'cryptography' not found; owner must install python3-cryptography (see CLAUDE.md Bootstrap)"

if [ ! -f "$DEST/$ZIP" ]; then
  [ -z "$(ls -A "$DEST" | grep -v '\.bad-')" ] || die "$DEST is not empty but incomplete; inspect and clear it"
  if [ -f "$DP753_LOCAL/$ZIP" ]; then
    echo "using offline copy: $DP753_LOCAL"
    cp -- "$DP753_LOCAL/$ZIP" "$DEST/.$ZIP.part" && mv -- "$DEST/.$ZIP.part" "$DEST/$ZIP"
  fi
fi
if [ ! -f "$DEST/$ZIP" ]; then
  dl="$(mktemp -d "$DEST/.dl.XXXXXX")"
  trap 'rm -rf "$dl"' EXIT
  python3 -I "$ROOT/scripts/mega-get.py" "$URL" "$dl" >/dev/null
  [ -f "$dl/$ZIP" ] || die "download did not produce '$ZIP'"
  mv -- "$dl/$ZIP" "$DEST/$ZIP"
  rm -rf "$dl"; trap - EXIT
fi
want_zip="$(grep -F "  $ZIP" "$SUMS" | cut -d' ' -f1)"
have_zip="$(sha256sum "$DEST/$ZIP" | cut -d' ' -f1)"
if [ "$have_zip" != "$want_zip" ]; then
  bad="$DEST/$ZIP.bad-$have_zip"
  mv -- "$DEST/$ZIP" "$bad"
  die "$ZIP hash mismatch ($have_zip); quarantined as '$bad' (delete it: rm -f -- '$bad'), then re-run"
fi

tmp="$(mktemp -d "$DEST/.x.XXXXXX")"
trap 'humount "$tmp/$HDA" >/dev/null 2>&1 || true; rm -rf "$tmp"' EXIT
unzip -q "$DEST/$ZIP" "$HDA" -d "$tmp"
hmount "$tmp/$HDA" >/dev/null
hcopy -r ":BlueSCSI PicoW Setup:DaynaPORT:$DP" "$tmp/$DP"
hcopy -m ":BlueSCSI PicoW Setup:MacTCP Setup:MacTCP" "$tmp/$TCP"
humount "$tmp/$HDA"
rm -f -- "$DEST/$DP" "$DEST/$TCP"
mv -- "$tmp/$DP" "$tmp/$TCP" "$DEST/"
check || die "unpacked files do not match $SUMS"
echo "fetched + verified: $DEST"
