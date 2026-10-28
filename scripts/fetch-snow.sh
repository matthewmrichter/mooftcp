#!/usr/bin/env bash
# Download the pinned Snow release, verify SHA-256, install to $MT_HOME/snow/snowemu.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
tag="$(tr -d '[:space:]' < "$root/tools/snow.version")"
want="$(awk '{print $1}' "$root/tools/snow.sha256")"
zipname="Snow.Linux.x86_64.zip"
dest="$MT_HOME/snow"
mkdir -p "$dest"

if [ -x "$dest/snowemu" ] && [ -f "$dest/VERSION" ] && [ "$(cat "$dest/VERSION")" = "$tag $want" ]; then
  echo "snowemu $tag already installed"; exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/$zipname" "https://github.com/twvd/snow/releases/download/$tag/$zipname"
got="$(sha256sum "$tmp/$zipname" | awk '{print $1}')"
if [ "$got" != "$want" ]; then
  echo "FATAL: SHA-256 mismatch for $zipname ($tag): expected $want, got $got" >&2
  exit 1
fi
unzip -q -o "$tmp/$zipname" -d "$tmp/x"
install -m 0755 "$tmp/x/snowemu" "$dest/snowemu"
echo "$tag $want" > "$dest/VERSION"
echo "installed snowemu $tag to $dest"
