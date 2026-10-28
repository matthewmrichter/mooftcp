#!/usr/bin/env bash
# Stage files for a real-hardware session (BlueSCSI shared folder).
#   stage-hw.sh <session-name> <file.bin>...
# Writes dist/hw/<session-name>/ with each file as MacBinary (.bin) and BinHex (.hqx,
# via macutils binhex), a README.txt (ASCII, CR line endings, for TeachText) and
# CHECKLIST.md rendered from docs/dev/hw-checklist-template.md.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
die() { echo "stage-hw.sh: $*" >&2; exit 1; }

[ $# -ge 2 ] || die "usage: stage-hw.sh <session-name> <file.bin>..."
session="$1"; shift
case "$session" in
  ""|.*|*[!A-Za-z0-9._-]*) die "session name must match [A-Za-z0-9._-] and not start with '.'";;
esac
command -v binhex >/dev/null || die "binhex (macutils) not installed"
for f in "$@"; do [ -f "$f" ] || die "no such file: $f"; done

out="$ROOT/dist/hw/$session"
[ ! -e "$out" ] || die "$out already exists; remove it or pick a new session name"
mkdir -p "$out"

names=()
for f in "$@"; do
  base="$(basename "$f")"
  case "$base" in *.bin) ;; *) die "$f is not a MacBinary .bin file";; esac
  stem="${base%.bin}"
  case "$stem" in *[!A-Za-z0-9._-]*) die "file name '$base' has characters other than [A-Za-z0-9._-]";; esac
  cp "$f" "$out/$base"
  binhex "$out/$base" > "$out/$stem.hqx" || die "binhex failed for $base"
  [ -s "$out/$stem.hqx" ] || die "binhex produced an empty file for $base"
  names+=("$stem")
done

# README.txt: ASCII, CR line endings.
readme_lf="$(mktemp)"; trap 'rm -f "$readme_lf"' EXIT
{
  echo "MoofTCP hardware session: $session"
  echo "Staged: $(date +%Y-%m-%d)"
  echo
  echo "Files in this folder (each in two formats):"
  for n in "${names[@]}"; do echo "  $n.bin   MacBinary"; echo "  $n.hqx   BinHex"; done
  echo
  echo "Copy ONE format of each file to the BlueSCSI shared folder on the SD card."
  echo "Try the .bin files first. If StuffIt Expander or the Finder shows a plain"
  echo "data file with no icon, use the .hqx file instead."
  echo
  echo "Then follow CHECKLIST.md (read it on the host; it lists the steps and"
  echo "what to report back)."
} > "$readme_lf"
if LC_ALL=C grep -q '[^ -~]' "$readme_lf"; then die "README contains non-ASCII"; fi
tr '\n' '\r' < "$readme_lf" > "$out/README.txt"

# CHECKLIST.md from template.
tpl="$ROOT/docs/dev/hw-checklist-template.md"
[ -f "$tpl" ] || die "missing template $tpl"
list=""
for n in "${names[@]}"; do list+="- \`$n.bin\` (or \`$n.hqx\`)"$'\n'; done
python3 -I - "$tpl" "$out/CHECKLIST.md" "$session" "$(date +%Y-%m-%d)" "$list" <<'PY'
import sys
tpl, dst, session, date, files = sys.argv[1:6]
s = open(tpl).read()
s = s.replace("{{SESSION}}", session).replace("{{DATE}}", date).replace("{{FILES}}", files.rstrip("\n"))
open(dst, "w").write(s)
PY
echo "staged $out"
ls -l "$out"
