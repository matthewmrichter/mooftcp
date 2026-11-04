#!/usr/bin/env bash
# Build the three release files (Plan 3 Task 11).
#   release.sh [--force] [--no-build] [--lite] <version>        e.g. release.sh 1.0rc1
# --lite: MoofTCP Lite (no DHCP client, docs/dev/RELEASE.md "Variants"): built in build-mac-lite/ (MT_LITE=1),
#   output dist/MoofTCP-Lite-<version>.{dsk,hqx,sit}, volume "MoofTCP Lite", application "MoofTCP Lite Installer".
#   Run once without and once with
#   --lite to make both variants; the two trees never mix.
# Output (dist/ is gitignored; existing outputs are refused without --force):
#   dist/MoofTCP-<version>.dsk   800K HFS image, volume "MoofTCP": "MoofTCP Installer" + "Read Me"
#   dist/MoofTCP-<version>.hqx   BinHex 4.0 of "MoofTCP Installer" (macutils binhex)
#   dist/MoofTCP-<version>.sit   StuffIt 1.5.1-compatible archive of both files, data+resource forks,
#                                type/creator and Finder flags (tools/sit/mksit, stored, no compression)
# Steps: Retro68 release build of the installer (MT_STACK_MARK=0, no MT_INST_DEBUG; build-mac.sh MoofInstaller
# also builds and embeds the MoofTCP control panel), checks that the binary is a release build, makes
# "Read Me" (docs/release/ReadMe.txt, CR line endings, TeachText ttro/ttxt), makes the three files,
# then runs scripts/release-verify.sh (independent readers). --no-build reuses build-mac/ as it is
# (still checks the release rules). Needs docker (Retro68 image), hfsutils, macutils, unar, gcc, python3.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
die() { echo "release.sh: $*" >&2; exit 1; }

force=""; nobuild=""; lite=""
while [ $# -gt 0 ]; do
  case "$1" in
    --force) force=1; shift;;
    --no-build) nobuild=1; shift;;
    --lite) lite=1; shift;;
    -*) die "unknown option $1";;
    *) break;;
  esac
done
[ $# -eq 1 ] || die "usage: release.sh [--force] [--no-build] [--lite] <version>"
if [ -n "$lite" ]; then export MT_LITE=1; else export MT_LITE=0; fi
. "$ROOT/scripts/variant.sh"     # BM: build-mac or build-mac-lite
ver="$1"
case "$ver" in ""|.*|*[!A-Za-z0-9._-]*) die "version must match [A-Za-z0-9._-] and not start with '.'";; esac
for t in docker hformat hmount hcopy hls humount binhex hexbin unar lsar gcc python3; do
  command -v "$t" >/dev/null || die "$t not installed (see docs/dev/RELEASE.md)"
done

dist="$ROOT/dist"
if [ -n "$lite" ]; then base="$dist/MoofTCP-Lite-$ver"; volname="MoofTCP Lite"; instname="MoofTCP Lite Installer"
else base="$dist/MoofTCP-$ver"; volname="MoofTCP"; instname="MoofTCP Installer"; fi
if [ -z "$force" ]; then
  for e in dsk hqx sit; do [ ! -e "$base.$e" ] || die "$base.$e exists; use --force to replace it"; done
fi
mkdir -p "$dist"
stage="$(mktemp -d "$dist/.stage-XXXXXX")"
trap 'rm -rf "$stage"' EXIT

# 1. Release build.
if [ -z "$nobuild" ]; then
  # MoofTCP first: on a fresh tree the installer's embed rule does not build it by itself
  MT_EXTRA_CFLAGS="-DMT_STACK_MARK=0" "$ROOT/scripts/build-mac.sh" MoofTCP
  MT_EXTRA_CFLAGS="-DMT_STACK_MARK=0" "$ROOT/scripts/build-mac.sh" MoofInstaller
fi
app="$ROOT/$BM/installer/MoofTCP Installer.bin"
[ -f "$app" ] || die "missing $app"

# 2. Release-build checks: no stack marks or paint, no installer debug hook, bundle bit set.
flags="$(grep -E '^CMAKE_C_FLAGS:' "$ROOT/$BM/CMakeCache.txt" || true)"
case "$flags" in *MT_STACK_MARK=1*|*MT_STACK_PAINT=*) die "$BM/ was configured with stack-mark flags ($flags); rebuild without --no-build";; esac
if [ -z "$nobuild" ]; then
  case "$flags" in *MT_STACK_MARK=0*) ;; *) die "$BM/ was not configured with -DMT_STACK_MARK=0 ($flags)";; esac
fi
# the variant switch of the tree must be the one asked for (MT_NO_DHCP ON = Lite)
nodhcp="$(sed -n 's/^MT_NO_DHCP:BOOL=\(.*\)$/\1/p' "$ROOT/$BM/CMakeCache.txt")"
[ "$nodhcp" = "$MT_NO_DHCP_OPT" ] || die "$BM/ has MT_NO_DHCP=$nodhcp, this release needs $MT_NO_DHCP_OPT"
for flt in MoofDrvr MoofInit; do
  [ -f "$ROOT/$BM/moof/$flt.flt.gdb" ] || die "missing $BM/moof/$flt.flt.gdb"
  # relative path: the container sees the repo at /src, not at its host path
  syms="$(cd "$ROOT" && scripts/r68 m68k-apple-macos-nm "$BM/moof/$flt.flt.gdb")" || die "nm failed on $flt.flt.gdb"
  [ "$(wc -l <<<"$syms")" -gt 5 ] || die "nm output of $flt.flt.gdb looks empty"
  if grep -q 'mt_os_stack_\(mark\|enter\|leave\)\|mt_os_mac_set_stack' <<<"$syms"; then
    die "$flt has stack-mark symbols: not a release build"
  fi
  if [ "$flt" = MoofDrvr ]; then   # Lite: no DHCP code at all; full: the client is there
    if [ -n "$lite" ]; then
      if grep -qi 'dhcp' <<<"$syms"; then die "MoofDrvr of the Lite tree has DHCP symbols"; fi
    else
      grep -q ' mt_dhcp_start$' <<<"$syms" || die "MoofDrvr of the full tree has no DHCP client"
    fi
  fi
done
if LC_ALL=C grep -aq 'MoofTCP Installer Debug' "$app"; then die "the installer contains the MT_INST_DEBUG hook"; fi
python3 -I - "$app" "$instname" <<'PY' || die "installer MacBinary check failed"
import struct, sys
d = open(sys.argv[1], "rb").read()
name, typ, cre, flags = d[2:2 + d[1]], d[65:69], d[69:73], (d[73] << 8) | d[101]
if (name, typ, cre) != (sys.argv[2].encode(), b"APPL", b"mtin") or not flags & 0x2000:
    sys.exit("identity/flags wrong: %r %r %r %04x" % (name, typ, cre, flags))
PY

python3 -I "$ROOT/scripts/relvers.py" ${lite:+--lite} "$ver" "$app" || die "the version strings in the build do not match $ver${lite:+ (Lite)}"

# 3. Stage: installer + "Read Me" as MacBinary.
cp "$app" "$stage/$instname.bin"
diaglog="$(sed -n 's/^DIAGLOG_TEXT=\([01]\)[[:space:]]*$/\1/p' "$ROOT/scripts/release.conf")"
case "$diaglog" in 0|1) ;; *) die "scripts/release.conf has no DIAGLOG_TEXT=0|1 line";; esac
python3 -I - "$ROOT/docs/release/ReadMe.txt" "$stage/Read Me.bin" "$diaglog" <<'PY'
import re, struct, sys, time
src, dst, diaglog = sys.argv[1:4]
txt = open(src, "rb").read()
if any(b > 126 or (b < 32 and b not in (10, 13)) for b in txt):
    sys.exit("ReadMe.txt must be plain ASCII")
if diaglog == "1":      # DIAGLOG_TEXT in scripts/release.conf: keep the block, drop the marker lines
    txt = re.sub(rb"(?m)^\[(BEGIN|END) DIAGNOSTIC LOG TEXT[^\n]*\n", b"", txt)
else:                   # drop the whole block, markers included
    txt = re.sub(rb"(?ms)^\[BEGIN DIAGNOSTIC LOG TEXT[^\n]*\n.*?^\[END DIAGNOSTIC LOG TEXT[^\n]*\n(\n){0,2}", b"", txt)
if not txt.endswith(b"\n"):
    txt += b"\n"
data = txt.replace(b"\r\n", b"\n").replace(b"\n", b"\r")
def crc16(b):
    c = 0
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c
now = int(time.time()) + 2082844800
h = bytearray(128)
nm = b"Read Me"
h[1] = len(nm); h[2:2 + len(nm)] = nm
h[65:69] = b"ttro"; h[69:73] = b"ttxt"
h[83:87] = struct.pack(">I", len(data))
h[91:95] = struct.pack(">I", now); h[95:99] = struct.pack(">I", now)
h[122] = 129; h[123] = 129
h[124:126] = struct.pack(">H", crc16(h[:124]))
open(dst, "wb").write(bytes(h) + data + b"\0" * (-len(data) % 128))
PY

# 4. .dsk: 800K HFS volume "MoofTCP" ("MoofTCP Lite" for Lite; hcopy -m keeps forks, type/creator and Finder flags).
dsk="$stage/out.dsk"
dd if=/dev/zero of="$dsk" bs=1024 count=800 status=none
hformat -l "$volname" "$dsk" >/dev/null
hmount "$dsk" >/dev/null
trap 'humount >/dev/null 2>&1 || true; rm -rf "$stage"' EXIT
hcopy -m "$stage/$instname.bin" ":$instname"
hcopy -m "$stage/Read Me.bin" ":Read Me"
hls -l :
humount >/dev/null
trap 'rm -rf "$stage"' EXIT

# 5. .hqx
# macutils binhex starts with its own comment "(This file must be converted; you knew that already.)",
# which unar does not recognise; replace it with the standard first line.
binhex "$stage/$instname.bin" > "$stage/body.hqx"
[ -s "$stage/body.hqx" ] || die "binhex produced an empty file"
{ echo "(This file must be converted with BinHex 4.0)"; sed '1{/^(/d;}' "$stage/body.hqx"; } > "$stage/out.hqx"

# 6. .sit (host-built writer, tools/sit)
sitdir="$ROOT/build/sit"
mkdir -p "$sitdir"
gcc -std=c99 -Wall -Wextra -O2 -I"$ROOT/tools/sit" -o "$sitdir/mksit" \
  "$ROOT/tools/sit/mksit.c" "$ROOT/tools/sit/classic_sit_writer.c" "$ROOT/tools/sit/stream.c" "$ROOT/tools/sit/sit_crc.c"
"$sitdir/mksit" "$stage/out.sit" "$stage/$instname.bin" "$stage/Read Me.bin"

# 7. Publish (replace only after all three were made), then verify.
mv -f "$stage/out.dsk" "$base.dsk"
mv -f "$stage/out.hqx" "$base.hqx"
mv -f "$stage/out.sit" "$base.sit"
ls -l "$base".dsk "$base".hqx "$base".sit
"$ROOT/scripts/release-verify.sh" ${lite:+--lite} "$ver"
