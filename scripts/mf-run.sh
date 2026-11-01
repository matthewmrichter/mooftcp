#!/usr/bin/env bash
# MultiFinder two-application run (Plan 3 Task 12): two copies of MfEcho (mac/mfecho) side by side,
# one quits while the other keeps echoing; MoofDiag runs while the long one is still connected.
#   mf-run.sh [--finder] [--base N] [--long A|B] [--release] [--pending] [--keep-disk] <label>
#     --finder     boot without MultiFinder (Set Startup: Finder) and run the two apps one after the other
#     --base N     testnet.py port base (echo at N+7; default 18000, started here when nothing listens)
#     --long A|B   which copy runs long (default A: MfEchoA, launched first, T120; the other T30 + a UDP
#                  stream); B = the first-launched one quits and the second keeps going
#     --pending    the short one quits with an async TCPRcv + UDPRead (completion routines) and ASRs
#                  installed, and the host answers 3 s after the quit (MfEcho option P, testnet "LATE")
#     --release    the short one releases its streams before quitting (default: it quits with them live,
#                  so the driver's per-application cleanup must forget them)
#     --keep-disk  do not run fresh-disk.sh / install-moof.sh
# Outputs: build-mac/mf/<label>/MfEchoA.out.txt, MfEchoB.out.txt, MoofDiag.out.txt, screenshots.
# Exit 0 when the long copy ends with RESULT PASS and its last table no longer lists the short copy's
# receive buffer (and, under MultiFinder, the driver counted the short copy's partition as gone).
set -euo pipefail
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="$MT_HOME/disks/work.hda"
die() { echo "mf-run.sh: $*" >&2; exit 1; }
usage() { echo "usage: mf-run.sh [--finder] [--base N] [--long A|B] [--release] [--pending] [--keep-disk] <label>" >&2; exit 2; }

mf=1; base=18000; long=A; rel=""; pend=""; keepdisk=""; label=""
while [ $# -gt 0 ]; do
  case "$1" in
    --finder) mf=""; shift;;
    --base) [ $# -ge 2 ] || usage; base="$2"; shift 2;;
    --long) [ $# -ge 2 ] || usage; long="$2"; shift 2;;
    --release) rel=R; shift;;
    --pending) pend=P; shift;;
    --keep-disk) keepdisk=1; shift;;
    -*) usage;;
    *) [ -z "$label" ] || usage; label="$1"; shift;;
  esac
done
[ -n "$label" ] || usage
case "$label" in *[!A-Za-z0-9._-]*) die "label may only contain A-Z a-z 0-9 . _ -";; esac
case "$long" in A|B) ;; *) usage;; esac
if [ "$long" = A ]; then cfgA="T120 B$base"; cfgB="T30 B$base U $rel $pend"; else cfgA="T30 B$base U $rel $pend"; cfgB="T120 B$base"; fi

cd "$ROOT"
out="$ROOT/build-mac/mf/$label"
rm -rf "$out"; mkdir -p "$out"
echo "== build"
scripts/build-mac.sh MfEcho >/dev/null
scripts/build-mac.sh MoofDiag >/dev/null
[ -n "$keepdisk" ] || scripts/build-mac.sh MoofTCP >/dev/null

started_testnet=""
if ! (exec 3<>/dev/tcp/127.0.0.1/$((base + 7))) 2>/dev/null; then
  scripts/testnet.py --base "$base" > "$out/testnet.log" 2>&1 &
  started_testnet=$!
  sleep 1
fi
cleanup() {
  scripts/snow.sh stop >/dev/null 2>&1 || true
  if [ -n "$started_testnet" ]; then kill "$started_testnet" 2>/dev/null || true; fi
}
trap cleanup EXIT

hfs_part() {
  python3 -I -c '
import sys
d = open(sys.argv[1], "rb").read(512 * 64)
n = 0
for i in range(1, 64):
    b = d[512*i:512*i+512]
    if b[:2] != b"PM":
        break
    if b[48:80].split(b"\0")[0] == b"Apple_HFS":
        n += 1
        print(n)
        break
' "$IMG"
}
with_volume() {
  local part vol rc=0
  part="$(hfs_part)"; [ -n "$part" ] || die "no Apple_HFS partition in $IMG"
  vol="$(hmount "$IMG" "$part" | sed -n 's/^Volume name is "\(.*\)"$/\1/p')"
  [ -n "$vol" ] || die "hmount failed"
  "$@" || rc=$?
  humount "$vol:" >/dev/null 2>&1 || true
  return $rc
}
put_apps() {
  local x
  for x in A B; do
    hdel ":MoofTCP Test:MfEcho$x" 2>/dev/null || true
    hdel ":MoofTCP Test:MfEcho$x.out" 2>/dev/null || true
    hdel ":MoofTCP Test:MfEcho" 2>/dev/null || true
    hcopy -m "$ROOT/build-mac/mfecho/MfEcho.bin" ":MoofTCP Test:"
    hrename ":MoofTCP Test:MfEcho" ":MoofTCP Test:MfEcho$x"
  done
  printf '%s' "$cfgA" > "$out/MfEchoA.cfg"; hcopy -t "$out/MfEchoA.cfg" ":MoofTCP Test:MfEchoA.cfg"
  printf '%s' "$cfgB" > "$out/MfEchoB.cfg"; hcopy -t "$out/MfEchoB.cfg" ":MoofTCP Test:MfEchoB.cfg"
  hdel ":MoofTCP Test:MoofDiag.out" 2>/dev/null || true
}
fetch() {
  local f
  for f in MfEchoA.out MfEchoB.out MoofDiag.out; do
    hcopy -t ":MoofTCP Test:$f" "$out/$f" 2>/dev/null && tr '\r' '\n' < "$out/$f" > "$out/$f.txt"
  done
  return 0
}

echo "== disk"
scripts/snow.sh stop >/dev/null 2>&1 || true
if [ -z "$keepdisk" ]; then
  scripts/fresh-disk.sh
  scripts/install-moof.sh >/dev/null
fi
scripts/deploy.sh build-mac/moofdiag/MoofDiag.bin --to ":MoofTCP Test:" >/dev/null
with_volume put_apps
python3 -I scripts/mf-startup.py "$IMG" "$([ -n "$mf" ] && echo multifinder || echo finder)"

export DISPLAY="${MT_DISPLAY:-:142}"; unset WAYLAND_DISPLAY
click() {  # click x y [dbl]
  local x=$1 y=$2 w i
  w="$(xdotool search --name Snow | head -1)"
  xdotool windowfocus "$w" 2>/dev/null || true
  for i in 1 2 3; do xdotool mousemove $((x-20+i*5)) $((y-20+i*5)); sleep 0.1; done
  xdotool mousemove "$x" "$y"; sleep 0.2
  xdotool mousedown 1; sleep 0.05; xdotool mouseup 1
  if [ "${3:-}" = dbl ]; then sleep 0.12; xdotool mousedown 1; sleep 0.05; xdotool mouseup 1; fi
}
shot() { scrot -o "$out/$1.png" >/dev/null 2>&1 || true; }
# finder: the Finder's menu bar is showing (dark menu titles at the left)
is_finder() {
  scrot -o "$out/state.png" >/dev/null 2>&1 || return 1
  python3 -W ignore - "$out/state.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("L")
bar = list(im.crop((40, 85, 430, 112)).getdata())
sys.exit(0 if sum(1 for p in bar if p < 100) > 20 else 1)
PY
}
# launch <x>: double-click the icon until the Finder's menu bar is gone (the app is frontmost)
launch() {
  local n
  for n in 1 2 3 4; do
    click "$1" 322 dbl; sleep 6
    is_finder || return 0
  done
  return 1
}
finder_front() {  # MultiFinder: a click into the Finder's folder window brings the Finder forward
  local n
  for n in 1 2 3; do
    click 600 480; sleep 3
    is_finder && return 0
  done
  return 1
}

echo "== run ($([ -n "$mf" ] && echo MultiFinder || echo Finder), long copy $long)"
scripts/snow.sh start >/dev/null
sleep 32
click 914 180 dbl; sleep 3          # MoofHD
click 341 290 dbl; sleep 4          # MoofTCP Test (icon view: MfEchoA, .cfg, MfEchoB, .cfg, MoofDiag)
shot 0-folder
if [ -n "$mf" ]; then
  launch 238 || die "MfEchoA did not start (screenshot $out/state.png)"
  shot 1-a-running
  finder_front || die "the Finder did not come forward"
  launch 484 || die "MfEchoB did not start"
  shot 2-both-running
  sleep 45                            # the short copy (T30) quits by itself
  finder_front || true
  shot 3-short-quit
  click 730 322 dbl; sleep 12         # MoofDiag while the long copy is still connected
  is_finder && { click 730 322 dbl; sleep 12; }
  shot 4-moofdiag
  click 766 348; sleep 3              # its "finished" alert
  sleep 80                            # the long copy (T120) ends
  shot 5-end
else
  # one application at a time: the short one first, then the long one (System 6 without MultiFinder)
  if [ "$long" = A ]; then first=484; second=238; else first=238; second=484; fi
  launch "$first" || die "the short copy did not start"
  sleep 40
  launch "$second" || die "the long copy did not start"
  sleep 130
  shot 5-end
fi
scripts/snow.sh stop >/dev/null
with_volume fetch
for f in MfEchoA MfEchoB; do [ -f "$out/$f.out.txt" ] || echo "missing $f.out"; done
[ -z "$mf" ] || [ -f "$out/MoofDiag.out.txt" ] || echo "missing MoofDiag.out"
L="MfEcho$long"; S="MfEcho$([ "$long" = A ] && echo B || echo A)"
[ -f "$out/$L.out.txt" ] && [ -f "$out/$S.out.txt" ] || die "outputs missing (see $out)"
grep -h "^RESULT" "$out/$L.out.txt" "$out/$S.out.txt"
shandle="$(sed -n "s/^TCPCreate 0 stream \([0-9a-f]*\).*/\1/p" "$out/$S.out.txt")"
# the short copy's own table line for that stream: handle + rbuf + A5 + connection serial (conn moves on
# at every open and at every forget), so a later stream in the same slot can never match it
sline="$(sed -n '/^connected: goodbye/,/^[^ ]/p' "$out/$S.out.txt" | grep "handle $shandle " | head -1 | sed 's/^ *tcp\[[0-9]*\] //')"
tail -n 30 "$out/$L.out.txt" | grep "^end: goodbye" || true
ok=1
grep -q "^RESULT PASS" "$out/$L.out.txt" || { echo "FAIL: the long copy lost echoes"; ok=""; }
[ -n "$sline" ] || { echo "FAIL: the short copy's stream line was not found"; ok=""; }
if [ -z "$rel" ] && [ -n "$sline" ] && sed -n '/^end: goodbye/,$p' "$out/$L.out.txt" | grep -qF "$sline"; then
  echo "FAIL: the short copy's stream ($sline) is still in the driver at the end"; ok=""
fi
if [ -z "$mf" ] && ! sed -n 's/^end: goodbye \([0-9]*\) .*/\1/p' "$out/$L.out.txt" | grep -q '^[1-9]'; then
  echo "FAIL: no goodbye kiss counted without MultiFinder"; ok=""
fi
if [ -n "$mf" ] && [ -z "$rel" ] && ! sed -n 's/^end: .* gone \([0-9]*\) .*/\1/p' "$out/$L.out.txt" | grep -q '^[1-9]'; then
  echo "FAIL: no owner counted as gone"; ok=""
fi
if [ -n "$pend" ]; then
  grep -h "^pending\|^TCPSend LATE\|^UDPWrite LATE" "$out/$S.out.txt" || true
  grep -q "late burst sent" "$out/testnet.log" 2>/dev/null && echo "host: late burst sent after the quit" || echo "host: (shared testnet, no log)"
  if [ -n "$mf" ]; then
    grep -E "^(owners|tcp\[|udp\[)" "$out/MoofDiag.out.txt" || true
    grep -q "^owners: .* gone [1-9]" "$out/MoofDiag.out.txt" || { echo "FAIL: MoofDiag shows no gone owner"; ok=""; }
  fi
fi
[ -n "$ok" ] || die "see $out"
echo "PASS ($out)"
