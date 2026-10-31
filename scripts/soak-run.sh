#!/usr/bin/env bash
# MoofTCP soak in Snow (Task 13): one boot, long-lived Flynn sessions plus UDP/ICMP/DNS churn, with
# MoofDiag/HeapInfo snapshots between the phases so counters and heap can be checked for drift.
#   soak-run.sh [--cycles N] [--type-secs S] [--probe-groups G] [--no-build] [--keep-disk]
#     --probe-groups G  ApiProbe groups of step 3 (default "67D"; "345" = bulk send/receive/burst for the tx-slot check)
# One cycle (about 7 minutes with the defaults):
#   1. MoofDiag            (appends the driver/Ethernet/core counters to MoofDiag.out)
#   2. Flynn               odd cycles: echo server 17007; even cycles: scripts/floodsrv.py on 17041 (60 s of bulk receive,
#                          then echo). One TCP connection per cycle, kept open while a line is typed every few seconds
#                          (single characters at 1 per 2 s, plus bursts), then Cmd-Q.
#   3. ApiProbe "67D"      UDP (group 6), ICMP echo (7) and the DNR suite (D): datagram, ping and DNS churn
#   4. HeapInfo            system-heap FreeMem/MaxMem/zone (appends to HeapInfo.out)
# Snow's NAT stops forwarding new guest TCP connections after about 16 per boot, so the soak uses one
# connection per cycle and the default 5 cycles. Only one app runs at a time (System 6, no MultiFinder).
# Afterwards (Snow stopped): MoofDiag.out, HeapInfo.out and ApiProbe.out are copied to build-mac/soak/, the new
# lines of snow.log are scanned for errors, and a drift summary is printed. Flynn screenshots at the end of each
# typing phase go to build-mac/soak/flynn-<n>.png. Exit status 0 when the run completed and the checks passed.
# Shares testnet.py (17007) with whoever runs it; starts its own only when nothing listens there.
set -euo pipefail
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="$MT_HOME/disks/work.hda"
DISPLAY_NUM="${MT_DISPLAY:-:142}"
die() { echo "soak-run.sh: $*" >&2; exit 1; }

cycles=5; typesecs=200; build=1; keepdisk=""; groups="67D"
while [ $# -gt 0 ]; do
  case "$1" in
    --cycles) cycles="$2"; shift 2;;
    --type-secs) typesecs="$2"; shift 2;;
    --probe-groups) groups="$2"; shift 2;;
    --no-build) build=""; shift;;
    --keep-disk) keepdisk=1; shift;;
    *) echo "usage: soak-run.sh [--cycles N] [--type-secs S] [--probe-groups G] [--no-build] [--keep-disk]" >&2; exit 2;;
  esac
done
cd "$ROOT"
out_dir="$ROOT/build-mac/soak"
mkdir -p "$out_dir"
shot="$out_dir/state.png"
rm -f "$out_dir"/*.out "$out_dir"/flynn-*.png
flynn_bin="${FLYNN_BIN:-$HOME/projects/flynn/build/Flynn.bin}"
[ -f "$flynn_bin" ] || die "no Flynn at $flynn_bin (FLYNN_BIN)"

if [ -n "$build" ]; then
  echo "== build"
  for t in HeapInfo ApiProbe MoofDiag MoofTCP; do scripts/build-mac.sh "$t" >/dev/null; done
fi

started=()
if ! (exec 3<>/dev/tcp/127.0.0.1/17007) 2>/dev/null; then
  scripts/testnet.py > "$out_dir/testnet.log" 2>&1 &
  started+=($!); sleep 1
fi
scripts/floodsrv.py --port 17041 --seconds 60 > "$out_dir/floodsrv.log" 2>&1 &
started+=($!); sleep 1
snow_stop() { scripts/snow.sh stop >/dev/null 2>&1 || true; }
cleanup() { snow_stop; for p in "${started[@]}"; do kill "$p" 2>/dev/null || true; done; }
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
# The files the apps write exist up front (empty) so the Finder's icon grid does not shift during the run.
put_files() {
  printf '%s' "$groups" > "$out_dir/ApiProbe.cfg"; : > "$out_dir/empty.txt"
  hcopy -t "$out_dir/ApiProbe.cfg" ":MoofTCP Test:ApiProbe.cfg"
  hcopy -t "$out_dir/empty.txt" ":MoofTCP Test:ApiProbe.out"
  hcopy -t "$out_dir/empty.txt" ":MoofTCP Test:HeapInfo.out"
  hcopy -t "$out_dir/empty.txt" ":MoofTCP Test:MoofDiag.out"
}
fetch_all() {
  hcopy -t ":MoofTCP Test:MoofDiag.out" "$out_dir/MoofDiag.out"
  hcopy -t ":MoofTCP Test:HeapInfo.out" "$out_dir/HeapInfo.out"
  hcopy -t ":MoofTCP Test:ApiProbe.out" "$out_dir/ApiProbe.out"
}

click() {  # click x y [dbl]
  local x=$1 y=$2 w i
  w="$(xdotool search --name Snow | head -1)"
  xdotool windowfocus "$w" 2>/dev/null || true
  for i in 1 2 3; do xdotool mousemove $((x-20+i*5)) $((y-20+i*5)); sleep 0.1; done
  xdotool mousemove "$x" "$y"; sleep 0.2
  xdotool mousedown 1; sleep 0.05; xdotool mouseup 1
  if [ "${3:-}" = dbl ]; then sleep 0.12; xdotool mousedown 1; sleep 0.05; xdotool mouseup 1; fi
}
screen_state() {  # finder | flynn | run | alert (a white dialog is up: ApiProbe/MoofDiag/HeapInfo alerts)
  scrot -o "$shot" >/dev/null 2>&1 || return 0
  python3 -W ignore - "$shot" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("L")
def white(box):
    px = list(im.crop(box).getdata())
    return sum(1 for p in px if p > 230) / len(px)
bar = list(im.crop((40, 85, 430, 112)).getdata())
bar2 = list(im.crop((430, 85, 475, 112)).getdata())
if sum(1 for p in bar2 if p < 100) > 10:
    print("flynn")        # Flynn's menu bar (Favorites) reaches past x 430; the Finder's ends at 405
elif sum(1 for p in bar if p < 100) > 20:
    print("finder")
elif white((130, 200, 300, 212)) > 0.95 and white((560, 330, 700, 390)) > 0.6:
    print("alert")
elif white((330, 240, 690, 340)) > 0.35:
    print("alert")
else:
    print("run")
PY
}
# Icon positions (x y) in the "MoofTCP Test" window with the 8 files deployed below (alphabetical grid).
declare -A ICON=([ApiProbe]="238 325" [Flynn]="607 325" [HeapInfo]="730 325" [MoofDiag]="238 447")
wait_state() {  # wait_state <state> <seconds>
  local t=0 st
  while [ "$t" -lt "$2" ]; do
    st="$(screen_state)"
    [ "$st" = "$1" ] && return 0
    sleep 2; t=$((t+2))
    scripts/snow.sh status >/dev/null 2>&1 || die "Snow died (at $(date +%T))"
  done
  return 1
}
launch() {  # launch <app>: double-click until the Finder bar is gone
  local app=$1 try
  for try in 1 2 3 4; do
    click ${ICON[$app]} dbl; sleep 3
    [ "$(screen_state)" = finder ] && click ${ICON[$app]} dbl && sleep 2
    [ "$(screen_state)" != finder ] && return 0     # (the Finder is the only state with its menu bar and no app)
  done
  scripts/snow.sh shot "$out_dir/$app.fail.png" >/dev/null 2>&1 || true
  die "could not launch $app"
}
run_alert_app() {  # run_alert_app <app> <timeout s>: launch, wait for its alert, Return, wait for the Finder
  local app=$1
  launch "$app"
  wait_state alert "$2" || { scripts/snow.sh shot "$out_dir/$app.fail.png" >/dev/null 2>&1 || true; die "$app did not finish (screenshot $out_dir/$app.fail.png)"; }
  sleep 1; xdotool key Return; sleep 2
  wait_state finder 30 || die "$app did not return to the Finder"
}
keys() { xdotool type --delay "$1" -- "$2"; }
flynn_phase() {  # flynn_phase <n> <port>
  local n=$1 port=$2 end i
  launch Flynn; sleep 5
  click 570 247
  for i in $(seq 1 24); do xdotool key BackSpace; done
  keys 120 "172.17.0.1"
  click 412 304 dbl
  for i in $(seq 1 8); do xdotool key BackSpace; done
  keys 120 "$port"
  click 719 519
  sleep 10
  scripts/snow.sh shot "$out_dir/flynn-$n-connected.png" >/dev/null 2>&1 || true
  end=$(( $(date +%s) + typesecs ))
  i=0
  while [ "$(date +%s)" -lt "$end" ]; do
    i=$((i+1))
    for c in a b c d e; do xdotool key "$c"; sleep 2; done     # five single characters, one per 2 s, each its own segment (xdotool type --delay would hold the key and autorepeat)
    keys 100 "line $i the quick brown fox 0123456789"   # a burst
    xdotool key Return
    sleep 6
  done
  sleep 5
  scripts/snow.sh shot "$out_dir/flynn-$n.png" >/dev/null 2>&1 || true
  xdotool keydown Super_L; sleep 0.2; xdotool key q; sleep 0.2; xdotool keyup Super_L
  sleep 3; xdotool key Return; sleep 2
  wait_state finder 30 || die "Flynn did not quit"
}

echo "== disk"
snow_stop
[ -n "$keepdisk" ] || scripts/fresh-disk.sh >/dev/null
scripts/install-moof.sh >/dev/null
scripts/deploy.sh build-mac/heapinfo/HeapInfo.bin build-mac/apiprobe/ApiProbe.bin build-mac/moofdiag/MoofDiag.bin "$flynn_bin" \
  --to ":MoofTCP Test:" >/dev/null
with_volume put_files
log_lines=0; [ -f "$MT_HOME/snow.log" ] && log_lines="$(wc -l < "$MT_HOME/snow.log")"

export DISPLAY="$DISPLAY_NUM"; unset WAYLAND_DISPLAY
echo "== run ($cycles cycles, $typesecs s of typing each)"
t0=$(date +%s)
scripts/snow.sh start
sleep 25
click 914 180 dbl; sleep 3; click 341 290 dbl; sleep 3
for n in $(seq 1 "$cycles"); do
  echo "-- cycle $n at +$(( $(date +%s) - t0 )) s"
  run_alert_app MoofDiag 120
  if [ $((n % 2)) = 1 ]; then flynn_phase "$n" 17007; else flynn_phase "$n" 17041; fi
  run_alert_app ApiProbe 600
  run_alert_app HeapInfo 120
done
run_alert_app MoofDiag 120
echo "== done after $(( $(date +%s) - t0 )) s of wall time"
sleep 2
snow_stop
with_volume fetch_all || die "could not read the outputs back"
tail -n +$((log_lines+1)) "$MT_HOME/snow.log" > "$out_dir/snow.log.new" 2>/dev/null || : > "$out_dir/snow.log.new"
echo "== checks"
python3 -I scripts/soak-check.py "$out_dir"
