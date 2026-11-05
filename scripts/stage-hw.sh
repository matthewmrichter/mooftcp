#!/usr/bin/env bash
# Stage files for a real-hardware session (BlueSCSI shared folder).
#   stage-hw.sh [--spec <file.md>] [--release <version>] <session-name> <file.bin>...
#   stage-hw.sh --verify <session-name>
# Stage: writes dist/hw/<session-name>/ with each file as MacBinary (.bin) and BinHex
# (.hqx, via macutils binhex), a README.txt (ASCII, CR line endings, for TeachText) and
# CHECKLIST.md rendered from scripts/hw-checklist-template.md. A failed run removes the
# half-made session directory. The staged session is verified automatically.
# --release <version> (Plan 3 Task 11) adds dist/MoofTCP-<version>.{dsk,hqx,sit} (scripts/release.sh; copied
# as they are, the .hqx is only decode-checked with hexbin) and the diagnostic apps from build-mac/ (HeapInfo,
# HeapInfoOpen, HeapInfoDNR, MoofDiag, ApiProbe, and KeyLat when built, as .bin + .hqx). Further <file.bin> arguments are optional then.
# The one release set carries both variants (docs/dev/RELEASE.md "Variants"). MoofDiag reads the driver's core
# state and must match the installed variant: the Lite one (build-mac-lite/) is staged too, as MoofDiag-Lite.bin
# (Mac name "MoofDiag Lite"); the other apps are the same for both variants.
# Spec file (markdown) supplies the owner-facing steps as "## Install", "## Try" and
# "## Undo" sections. Without --spec those sections say no steps were given.
# Verify: round-trips every .hqx with hexbin and checks it equals the .bin (name,
# type/creator, flags, data and resource forks); checks README.txt is ASCII with CR only;
# fails if the checklist still has TODO-FILL or {{ markers.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
die() { echo "stage-hw.sh: $*" >&2; exit 1; }

PYCMP='
import struct, sys
def parse(p):
    d = open(p, "rb").read()
    nl = d[1]
    dl, rl = struct.unpack(">II", d[83:91])
    off = 128 + ((dl + 127) // 128) * 128
    return dict(name=d[2:2+nl], type=d[65:69], creator=d[69:73], flags=(d[73], d[101]),
                data=d[128:128+dl], rsrc=d[off:off+rl])
a, b = parse(sys.argv[1]), parse(sys.argv[2])
bad = [k for k in a if a[k] != b[k]]
if bad:
    print("differs in: " + ", ".join(bad)); sys.exit(1)
print("%s type/creator %s/%s data %d rsrc %d: equal" % (a["name"].decode("mac_roman"),
      a["type"].decode("ascii"), a["creator"].decode("ascii"), len(a["data"]), len(a["rsrc"])))
'

verify_session() {
  local dir="$1" n=0 hqx stem tmp
  [ -d "$dir" ] || die "no such session dir: $dir"
  command -v hexbin >/dev/null || die "hexbin (macutils) not installed"
  tmp="$(mktemp -d)"
  for hqx in "$dir"/*.hqx; do
    [ -e "$hqx" ] || die "no .hqx files in $dir"
    stem="$(basename "$hqx" .hqx)"
    mkdir "$tmp/$stem"
    (cd "$tmp/$stem" && hexbin "$hqx" >/dev/null) || { rm -rf "$tmp"; die "hexbin failed on $stem.hqx"; }
    set -- "$tmp/$stem"/*.bin
    [ -f "$1" ] || { rm -rf "$tmp"; die "hexbin of $stem.hqx produced no MacBinary file"; }
    if [ -f "$dir/$stem.bin" ]; then
      python3 -I -c "$PYCMP" "$1" "$dir/$stem.bin" || { rm -rf "$tmp"; die "$stem: round-trip mismatch"; }
    else   # a release .hqx has no .bin of its own (it is the installer's BinHex): decode check only
      echo "$stem.hqx decodes with hexbin (no matching .bin; scripts/release-verify.sh compares it)"
    fi
    n=$((n+1))
  done
  rm -rf "$tmp"
  [ -f "$dir/README.txt" ] || die "missing README.txt"
  if LC_ALL=C grep -q '[^ -~'$'\r'']' "$dir/README.txt"; then die "README.txt is not plain ASCII"; fi
  [ "$(tr -cd '\n' < "$dir/README.txt" | wc -c)" -eq 0 ] || die "README.txt has LF; must be CR only"
  [ -f "$dir/CHECKLIST.md" ] || die "missing CHECKLIST.md"
  if grep -q 'TODO-FILL\|{{' "$dir/CHECKLIST.md"; then die "CHECKLIST.md still has TODO-FILL or {{ markers"; fi
  echo "verified $n file(s) in $dir"
}

if [ "${1:-}" = "--verify" ]; then
  [ $# -eq 2 ] || die "usage: stage-hw.sh --verify <session-name>"
  verify_session "$ROOT/dist/hw/$2"
  exit 0
fi

spec=""; release=""
while [ $# -gt 0 ]; do
  case "$1" in
    --spec)
      [ $# -ge 2 ] || die "--spec needs a file"
      spec="$2"; shift 2
      [ -f "$spec" ] || die "no such spec: $spec";;
    --release)
      [ $# -ge 2 ] || die "--release needs a version"
      release="$2"; shift 2;;
    --variant)
      die "--variant is gone: the one release set carries both variants (docs/dev/RELEASE.md \"Variants\")";;
    *) break;;
  esac
done
if [ -n "$release" ]; then
  [ $# -ge 1 ] || die "usage: stage-hw.sh [--spec file.md] [--release ver] <session-name> [file.bin]..."
else
  [ $# -ge 2 ] || die "usage: stage-hw.sh [--spec file.md] [--release ver] <session-name> <file.bin>..."
fi
session="$1"; shift
rel_files=(); tmpbin=""
if [ -n "$release" ]; then
  case "$release" in ""|.*|*[!A-Za-z0-9._-]*) die "bad release version";; esac
  apps="build-mac"
  for e in dsk hqx sit; do rel_files+=("$ROOT/dist/MoofTCP-$release.$e"); done
  set -- "$@" "$ROOT/build-mac/moofdiag/MoofDiag.bin"
  lt="$ROOT/build-mac-lite/moofdiag/MoofDiag.bin"
  [ -f "$lt" ] || die "missing $lt (MT_LITE=1 scripts/build-mac.sh MoofDiag)"
  tmpbin="$(mktemp -d)"
  trap 'rm -rf "$tmpbin"' EXIT          # replaced by cleanup() below, which removes it too
  cp "$lt" "$tmpbin/MoofDiag-Lite.bin"
  python3 -I "$ROOT/scripts/mbflags.py" "$tmpbin/MoofDiag-Lite.bin" 0x0000 "MoofDiag Lite" >/dev/null
  set -- "$@" "$tmpbin/MoofDiag-Lite.bin"
  for f in "$ROOT/$apps/heapinfo/HeapInfo.bin" "$ROOT/$apps/heapinfo/HeapInfoOpen.bin" \
           "$ROOT/$apps/heapinfo/HeapInfoDNR.bin" "$ROOT/$apps/apiprobe/ApiProbe.bin"; do set -- "$@" "$f"; done
  # KeyLat (mac/keylat, keypress-to-wire latency, docs/dev/HARDWARE.md "KeyLat") rides along when it has been built
  [ ! -f "$ROOT/$apps/keylat/KeyLat.bin" ] || set -- "$@" "$ROOT/$apps/keylat/KeyLat.bin"
  for f in "${rel_files[@]}"; do [ -f "$f" ] || die "missing $f (run scripts/release.sh $release)"; done
fi
case "$session" in
  ""|.*|*[!A-Za-z0-9._-]*) die "session name must match [A-Za-z0-9._-] and not start with '.'";;
esac
command -v binhex >/dev/null || die "binhex (macutils) not installed"
for f in "$@"; do [ -f "$f" ] || die "no such file: $f"; done

out="$ROOT/dist/hw/$session"
[ ! -e "$out" ] || die "$out already exists; remove it or pick a new session name"
done_ok=""; readme_lf=""
cleanup() {
  if [ -z "$done_ok" ]; then rm -rf "$out"; fi
  if [ -n "$readme_lf" ]; then rm -f "$readme_lf"; fi
  if [ -n "$tmpbin" ]; then rm -rf "$tmpbin"; fi
}
trap cleanup EXIT
mkdir -p "$out"

names=(); relnames=()
for f in "${rel_files[@]}"; do
  cp "$f" "$out/$(basename "$f")"; relnames+=("$(basename "$f")")
done
for f in "$@"; do
  base="$(basename "$f")"
  case "$base" in *.bin) ;; *) die "$f is not a MacBinary .bin file";; esac
  stem="${base%.bin}"
  case "$stem" in *[!A-Za-z0-9._-]*) die "file name '$base' has characters other than [A-Za-z0-9._-]";; esac
  cp "$f" "$out/$base"
  # macutils' own first comment line is replaced by the standard one so unar can list the file
  binhex "$out/$base" > "$out/$stem.body" || die "binhex failed for $base"
  { echo "(This file must be converted with BinHex 4.0)"; sed '1{/^(/d;}' "$out/$stem.body"; } > "$out/$stem.hqx"
  rm -f "$out/$stem.body"
  [ -s "$out/$stem.hqx" ] || die "binhex produced an empty file for $base"
  names+=("$stem")
done

# README.txt: ASCII, CR line endings.
readme_lf="$(mktemp)"
{
  echo "MoofTCP hardware session: $session"
  echo "Staged: $(date +%Y-%m-%d)"
  echo
  echo "Files in this folder (each in two formats):"
  for n in "${relnames[@]}"; do echo "  $n   release file (already in its final format)"; done
  for n in "${names[@]}"; do echo "  $n.bin   MacBinary"; echo "  $n.hqx   BinHex"; done
  echo
  echo "Copy ONE format of each file to the BlueSCSI shared folder on the SD card."
  echo "Try the .bin files first. If StuffIt Expander or the Finder shows a plain"
  echo "data file with no icon, use the .hqx file instead."
  echo
  echo "Then follow CHECKLIST.md (read it on the host; it lists the steps and"
  echo "what to report back)."
} > "$readme_lf"
tr '\n' '\r' < "$readme_lf" > "$out/README.txt"

# CHECKLIST.md from template + optional spec.
tpl="$ROOT/scripts/hw-checklist-template.md"
[ -f "$tpl" ] || die "missing template $tpl"
[ -n "$spec" ] || echo "stage-hw.sh: warning: no --spec; Install/Try/Undo will say no steps were given" >&2
list=""
for n in "${relnames[@]}"; do list+="- \`$n\`"$'\n'; done
for n in "${names[@]}"; do list+="- \`$n.bin\` (or \`$n.hqx\`)"$'\n'; done
python3 -I - "$tpl" "$out/CHECKLIST.md" "$session" "$(date +%Y-%m-%d)" "$list" "$spec" <<'PY'
import re, sys
tpl, dst, session, date, files, spec = sys.argv[1:7]
none = "No steps were given for this session."
secs = {"Install": none, "Try": none, "Undo": none}
if spec:
    text = open(spec).read()
    parts = re.split(r"(?m)^## +(.+?) *$", text)
    found = dict(zip(parts[1::2], parts[2::2]))
    for k in secs:
        if k not in found or not found[k].strip():
            sys.exit("spec has no '## %s' section" % k)
        secs[k] = found[k].strip()
s = open(tpl).read()
for key, val in (("SESSION", session), ("DATE", date), ("FILES", files.rstrip("\n")),
                 ("INSTALL", secs["Install"]), ("TRY", secs["Try"]), ("UNDO", secs["Undo"])):
    s = s.replace("{{%s}}" % key, val)
open(dst, "w").write(s)
PY
verify_session "$out"
done_ok=1
echo "staged $out"
ls -l "$out"
