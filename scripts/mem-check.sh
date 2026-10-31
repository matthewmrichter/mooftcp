#!/usr/bin/env bash
# MoofTCP resident-memory cap check (Task 13; rules in docs/dev/EMULATOR.md "The cap").
#   mem-check.sh [--keep-disk] [--no-build]
# One Snow boot on a fresh work.hda with MoofTCP installed (scripts/install-moof.sh), then in order:
#   HeapInfo (idle), HeapInfoOpen (driver open), ApiProbe groups 2+3 (real TCP traffic: the "network
#   operation"), HeapInfo again, HeapInfoDNR twice. Each app appends exact byte figures to
#   :MoofTCP Test:HeapInfo.out (the alerts stay; they are clicked away). The figures are read back
#   with Snow stopped and the three rules are printed as PASS/FAIL:
#     1. occupied (zone size - FreeMem) over the no-network baseline 149,248, after the driver open
#        and after the network operation, <= 195,046
#     2. system-zone growth over the no-network baseline 162,100 <= 273,672
#     3. HeapInfoDNR (second run): app heap taken <= 29,150 and system heap taken <= 0
# Exit status 0 only when every rule passes. Output also in build-mac/memcheck/MemCheck.out.
# Uses snow.sh, fresh-disk.sh, install-moof.sh, deploy.sh; starts scripts/testnet.py only when nothing
# listens on 17007 (another session's testnet is shared, never killed).
set -euo pipefail
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="$MT_HOME/disks/work.hda"
DISPLAY_NUM="${MT_DISPLAY:-:142}"
die() { echo "mem-check.sh: $*" >&2; exit 1; }

keepdisk=""; build=1
while [ $# -gt 0 ]; do
  case "$1" in
    --keep-disk) keepdisk=1; shift;;
    --no-build) build=""; shift;;
    *) echo "usage: mem-check.sh [--keep-disk] [--no-build]" >&2; exit 2;;
  esac
done
cd "$ROOT"
out_dir="$ROOT/build-mac/memcheck"
mkdir -p "$out_dir"
shot="$out_dir/state.png"
res="$out_dir/HeapInfo.out"
rm -f "$res" "$out_dir/MemCheck.out"

if [ -n "$build" ]; then
  echo "== build"
  for t in HeapInfo HeapInfoOpen HeapInfoDNR ApiProbe MoofTCP; do scripts/build-mac.sh "$t" >/dev/null; done
fi
for f in build-mac/heapinfo/HeapInfo.bin build-mac/heapinfo/HeapInfoOpen.bin build-mac/heapinfo/HeapInfoDNR.bin \
         build-mac/apiprobe/ApiProbe.bin build-mac/moof/MacTCP.bin; do
  [ -f "$f" ] || die "missing $f"
done

started_testnet=""
if ! (exec 3<>/dev/tcp/127.0.0.1/17007) 2>/dev/null; then
  scripts/testnet.py > "$out_dir/testnet.log" 2>&1 &
  started_testnet=$!
  sleep 1
fi
snow_stop() { scripts/snow.sh stop >/dev/null 2>&1 || true; }
cleanup() { snow_stop; if [ -n "$started_testnet" ]; then kill "$started_testnet" 2>/dev/null || true; fi; }
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
# The files the apps write are created up front (empty) so the Finder's icon grid does not shift during the run.
put_cfg() {
  printf '23' > "$out_dir/ApiProbe.cfg"; : > "$out_dir/empty.txt"
  hcopy -t "$out_dir/ApiProbe.cfg" ":MoofTCP Test:ApiProbe.cfg"
  hcopy -t "$out_dir/empty.txt" ":MoofTCP Test:ApiProbe.out"
  hcopy -t "$out_dir/empty.txt" ":MoofTCP Test:HeapInfo.out"
}
fetch_res() { hcopy -t ":MoofTCP Test:HeapInfo.out" "$res"; }

click() {  # click x y [dbl]
  local x=$1 y=$2 w i
  w="$(xdotool search --name Snow | head -1)"
  xdotool windowfocus "$w" 2>/dev/null || true
  for i in 1 2 3; do xdotool mousemove $((x-20+i*5)) $((y-20+i*5)); sleep 0.1; done
  xdotool mousemove "$x" "$y"; sleep 0.2
  xdotool mousedown 1; sleep 0.05; xdotool mouseup 1
  if [ "${3:-}" = dbl ]; then sleep 0.12; xdotool mousedown 1; sleep 0.05; xdotool mouseup 1; fi
}
screen_state() {  # finder | run | done (ApiProbe's wide box) | alert (a small alert/dialog is up)
  scrot -o "$shot" >/dev/null 2>&1 || return 0
  python3 -W ignore - "$shot" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("L")
def white(box):
    px = list(im.crop(box).getdata())
    return sum(1 for p in px if p > 230) / len(px)
bar = list(im.crop((40, 85, 430, 112)).getdata())
if sum(1 for p in bar if p < 100) > 20:
    print("finder")
elif white((130, 200, 300, 212)) > 0.95 and white((560, 330, 700, 390)) > 0.6:
    print("done")
elif white((330, 240, 690, 340)) > 0.35:
    print("alert")
else:
    print("run")
PY
}
# Icon positions (x y) in the "MoofTCP Test" window with the 7 files deployed above (grid is alphabetical).
declare -A ICON=([ApiProbe]="238 325" [HeapInfo]="607 325" [HeapInfoDNR]="114 447" [HeapInfoOpen]="238 447")
open_folder() { click 914 180 dbl; sleep 3; click 341 290 dbl; sleep 3; }
wait_state() {  # wait_state <state> <seconds>
  local t=0
  while [ "$t" -lt "$2" ]; do
    st="$(screen_state)"
    [ "$st" = "$1" ] && return 0
    [ "$1" = alert ] && [ "$st" = done ] && return 0   # the heap alerts also fit the "done" box test
    sleep 2; t=$((t+2))
    scripts/snow.sh status >/dev/null 2>&1 || die "Snow died"
  done
  return 1
}
run_alert_app() {  # run_alert_app <app> <alerts>: launch from the Finder, click OK <alerts> times, back to the Finder
  local app=$1 n=$2 k try
  for try in 1 2 3 4; do
    click ${ICON[$app]} dbl; sleep 3
    [ "$(screen_state)" = finder ] && click ${ICON[$app]} dbl
    if wait_state alert 90; then break; fi
    echo "  $app: no alert yet (try $try)"
    [ "$(screen_state)" = finder ] || continue
  done
  case "$(screen_state)" in alert|done) ;; *) scripts/snow.sh shot "$out_dir/$app.fail.png" >/dev/null 2>&1 || true; die "$app showed no alert";; esac
  for k in $(seq 1 "$n"); do
    sleep 1; xdotool key Return; sleep 2
  done
  wait_state finder 30 || die "$app did not return to the Finder"
  echo "  $app done"
}

echo "== disk"
snow_stop
[ -n "$keepdisk" ] || scripts/fresh-disk.sh >/dev/null
scripts/install-moof.sh >/dev/null
scripts/deploy.sh build-mac/heapinfo/HeapInfo.bin build-mac/heapinfo/HeapInfoOpen.bin build-mac/heapinfo/HeapInfoDNR.bin \
  build-mac/apiprobe/ApiProbe.bin --to ":MoofTCP Test:" >/dev/null
with_volume put_cfg

export DISPLAY="$DISPLAY_NUM"; unset WAYLAND_DISPLAY
echo "== run"
scripts/snow.sh start
sleep 25
open_folder
run_alert_app HeapInfo 1          # idle, driver not yet opened
run_alert_app HeapInfoOpen 1      # OpenDriver(.IPP)
echo "  ApiProbe (groups 2 3: network operation)"
click ${ICON[ApiProbe]} dbl; sleep 4
[ "$(screen_state)" = finder ] && click ${ICON[ApiProbe]} dbl
wait_state done 600 || die "ApiProbe did not finish"
xdotool key Return; wait_state finder 30 || die "ApiProbe did not quit"
run_alert_app HeapInfo 1          # after the network operation
run_alert_app HeapInfoDNR 2       # first DNR run
run_alert_app HeapInfoDNR 2       # second DNR run
run_alert_app HeapInfo 1          # after the resolver
sleep 2
snow_stop
with_volume fetch_res || die "HeapInfo.out missing on the image"
echo "== figures"
python3 -I - "$res" "$out_dir/MemCheck.out" <<'PY'
import sys
BASE_OCC, BASE_ZONE = 149248, 162100
CAP1, CAP2, CAP3_APP, CAP3_SYS = 195046, 273672, 29150, 0
rows = [l.split() for l in open(sys.argv[1]).read().replace("\r", "\n").split("\n") if l.strip()]
heap = [r for r in rows if r[0] == "HEAP"]
opn = [r for r in rows if r[0] == "OPEN"]
dnr = [r for r in rows if r[0] == "DNR"]
out = []
def P(s): print(s); out.append(s)
ok = True
def chk(name, val, cap):
    global ok
    good = val <= cap
    ok &= good
    P("%-4s %-52s %9d  (cap %d)" % ("PASS" if good else "FAIL", name, val, cap))
if len(heap) < 3 or len(opn) < 1 or len(dnr) < 2:
    P("FAIL missing figures: %d HEAP, %d OPEN, %d DNR lines" % (len(heap), len(opn), len(dnr)))
    sys.exit(1)
h0, h1, h2 = heap[0], heap[1], heap[2]
o = opn[0]
f = lambda r, i: int(r[i])
P("idle (before open):      FreeMem %d MaxMem %d zone %d occupied %d (over baseline %d)" % (f(h0,1), f(h0,2), f(h0,3), f(h0,3)-f(h0,1), f(h0,3)-f(h0,1)-BASE_OCC))
P("HeapInfoOpen:            OpenDriver err %d FreeMem %d -> %d zone %d (driver open costs %d)" % (f(o,1), f(o,2), f(o,3), f(o,4), f(o,2)-f(o,3)))
occ_open = f(o,4) - f(o,3)
P("after network op:        FreeMem %d MaxMem %d zone %d" % (f(h1,1), f(h1,2), f(h1,3)))
occ_net = f(h1,3) - f(h1,1)
P("after resolver:          FreeMem %d MaxMem %d zone %d" % (f(h2,1), f(h2,2), f(h2,3)))
occ_dnr = f(h2,3) - f(h2,1)
if f(o,1) != 0: ok = False; P("FAIL OpenDriver returned %d" % f(o,1))
chk("rule 1  occupied over baseline, after driver open", occ_open - BASE_OCC, CAP1)
chk("rule 1  occupied over baseline, after network op", occ_net - BASE_OCC, CAP1)
chk("rule 1  occupied over baseline, after resolver", occ_dnr - BASE_OCC, CAP1)
chk("rule 2  system-zone growth over baseline", f(h1,3) - BASE_ZONE, CAP2)
for i, d in enumerate(dnr):
    P("HeapInfoDNR run %d:      open rc %s lookup rc %s  sys taken open/looked-up/closed %s/%s/%s  app %s/%s/%s  addr %08x" % (
        i+1, d[1], d[2], d[3], d[5], d[7], d[4], d[6], d[8], int(d[9]) & 0xffffffff))
d = dnr[-1]
if int(d[1]) != 0 or int(d[2]) != 0: ok = False; P("FAIL resolver open/lookup did not succeed")
chk("rule 3  DNR app heap taken, open after lookup (2nd run)", int(d[6]), CAP3_APP)
chk("rule 3  DNR system heap taken, open after lookup (2nd run)", int(d[5]), CAP3_SYS)
chk("rule 3  DNR heap not returned at close (leak), app", int(d[8]), 0)
P("RESULT %s" % ("PASS" if ok else "FAIL"))
open(sys.argv[2], "w").write("\n".join(out) + "\n")
sys.exit(0 if ok else 1)
PY
