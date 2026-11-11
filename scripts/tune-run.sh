#!/usr/bin/env bash
# MoofTCPTune check in Snow (MT_TUNE task 14): install the perf build of MoofTCP (MT_TUNE=1: build-mac-tune/), run
# MoofTCPTune and then MoofDiag in ONE boot, both unattended with the run file "!!<id>", and print both outputs.
#   tune-run.sh [--keep-disk] [--cfg <MoofTCPTune.cfg>] [--release] <id>
#     <id>        the run id: a preset (default delack0 delack20 delack50 qack16 qackall rto500 rto700 stall2 stall5),
#                 or any other id with --cfg
#     --cfg f     also put f on the image as MoofTCPTune.cfg (read when the id is no preset)
#     --release   use the RELEASE MoofTCP instead (build-mac/): MoofTCPTune must then end RESULT FAIL "not a tune
#                 build" and MoofDiag prints no tune: line; this run exits 0 when exactly that happened
# Checks (exit 0 only if all hold): MoofTCPTune-<id>.out ends RESULT OK and its after: line is the preset's values,
# MoofDiag-<id>.out ends RESULT PASS and its tune: line equals the after: line. Outputs: build-mac-tune/tune/.
# The levers last until the restart, so a second run id needs a second boot (the run file cannot change while
# Snow runs). Run as: flock /tmp/mooftcp-snow.lock scripts/tune-run.sh <id>
set -euo pipefail
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="$MT_HOME/disks/work.hda"
DISPLAY_NUM="${MT_DISPLAY:-:142}"
TIMEOUT="${TUNE_TIMEOUT:-300}"
die() { echo "tune-run.sh: $*" >&2; exit 1; }

keepdisk=""; cfg=""; release=""; runid=""
while [ $# -gt 0 ]; do
  case "$1" in
    --keep-disk) keepdisk=1; shift;;
    --cfg) [ $# -ge 2 ] || die "--cfg needs a file"; cfg="$2"; shift 2;;
    --release) release=1; shift;;
    -*) echo "usage: tune-run.sh [--keep-disk] [--cfg <file>] [--release] <id>" >&2; exit 2;;
    *) runid="$1"; shift;;
  esac
done
[ -n "$runid" ] || { echo "usage: tune-run.sh [--keep-disk] [--cfg <file>] [--release] <id>" >&2; exit 2; }
cd "$ROOT"
if [ -n "$release" ]; then BM=build-mac; unset MT_TUNE; else BM=build-mac-tune; export MT_TUNE=1; fi   # as scripts/variant.sh
out_dir="$ROOT/$BM/tune"
mkdir -p "$out_dir"
echo "== build ($BM)"
for t in MoofTCP MoofDiag MoofTCPTune; do scripts/build-mac.sh "$t" >/dev/null; done
out_tune="$out_dir/MoofTCPTune-$runid.out"; out_diag="$out_dir/MoofDiag-$runid.out"
shot="$out_dir/tune.png"
rm -f "$out_tune" "$out_diag" "$shot"
snow_stop() { scripts/snow.sh stop >/dev/null 2>&1 || true; }
trap snow_stop EXIT
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
with_volume() {  # with_volume <cmd...>: mount work.hda (Snow stopped), run, unmount
  local part vol rc=0
  part="$(hfs_part)"; [ -n "$part" ] || die "no Apple_HFS partition in $IMG"
  vol="$(hmount "$IMG" "$part" | sed -n 's/^Volume name is "\(.*\)"$/\1/p')"
  [ -n "$vol" ] || die "hmount failed"
  "$@" || rc=$?
  humount "$vol:" >/dev/null 2>&1 || true
  return $rc
}
fetch_outs() { hcopy -t ":MoofTCP Test:MoofTCPTune-$runid.out" "$out_tune" && hcopy -t ":MoofTCP Test:MoofDiag-$runid.out" "$out_diag"; }
put_runfile() { printf '!!%s' "$runid" > "$1" && hcopy -t "$1" ":MoofTCP Test:Agent Run"; }
del_outs() { hdel ":MoofTCP Test:MoofTCPTune-$runid.out" 2>/dev/null || true; hdel ":MoofTCP Test:MoofDiag-$runid.out" 2>/dev/null || true; hdel ":MoofTCP Test:Agent Run" 2>/dev/null || true; hdel ":MoofTCP Test:MoofTCPTune.cfg" 2>/dev/null || true; }
put_cfg() { hcopy -t "$cfg" ":MoofTCP Test:MoofTCPTune.cfg"; }

click() {  # click x y [dbl]
  local x=$1 y=$2 w i
  w="$(xdotool search --name Snow | head -1)"
  xdotool windowfocus "$w" 2>/dev/null || true
  for i in 1 2 3; do xdotool mousemove $((x-20+i*5)) $((y-20+i*5)); sleep 0.1; done
  xdotool mousemove "$x" "$y"; sleep 0.2
  xdotool mousedown 1; sleep 0.05; xdotool mouseup 1
  if [ "${3:-}" = dbl ]; then sleep 0.12; xdotool mousedown 1; sleep 0.05; xdotool mouseup 1; fi
}
screen_state() {  # finder | run | done | crash (see probe-run.sh)
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
    print("crash")
else:
    print("run")
PY
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
screen_state() {  # finder | run | done | crash (see probe-run.sh)
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
    print("crash")
else:
    print("run")
PY
}
# The folder ":MoofTCP Test:" lists (by name) Agent Run, MoofDiag, MoofTCPTune [, MoofTCPTune.cfg first]: icons 123 px apart.
open_folder() { click 914 180 dbl; sleep 3; click 341 290 dbl; sleep 3; }
wait_quit() {  # wait for the unattended app to end: the Finder is back twice in a row
  local t=0 seen=0 st
  sleep 5; t=5
  while [ "$t" -lt "$TIMEOUT" ]; do
    sleep 5; t=$((t+5))
    scripts/snow.sh status >/dev/null 2>&1 || die "Snow died"
    st="$(screen_state)"
    [ "$st" = done ] && die "$1 showed its closing alert (the run file was not honoured)"
    [ "$st" = crash ] && { scripts/snow.sh shot "$shot" >/dev/null 2>&1 || true; die "a system error dialog was on the screen (screenshot $shot)"; }
    if [ "$st" = finder ]; then seen=$((seen+1)); else seen=0; fi
    [ "$seen" -ge 2 ] && { echo "$1 quit after about ${t}s"; return 0; }
  done
  die "$1 did not quit within ${TIMEOUT}s"
}
launch() {  # launch <x>: select the icon at x, then double click it ONCE (an unattended app quits within seconds, a
            # second double click would run it again and its output would show the first run's levers as "before")
  click "$1" 322; sleep 1; click "$1" 322 dbl; sleep 3
}

echo "== disk"
snow_stop
[ -n "$keepdisk" ] || scripts/fresh-disk.sh
scripts/install-moof.sh >/dev/null   # the MoofTCP of this tree ($BM; MT_TUNE as exported above)
scripts/deploy.sh "$BM/moofdiag/MoofDiag.bin" "$BM/mooftcptune/MoofTCPTune.bin" --to ":MoofTCP Test:" >/dev/null
with_volume del_outs
rf="$(mktemp)"; with_volume put_runfile "$rf" || die "could not write the run file"; rm -f "$rf"
[ -z "$cfg" ] || with_volume put_cfg || die "could not write the cfg file"

export DISPLAY="$DISPLAY_NUM"; unset WAYLAND_DISPLAY
echo "== run"
scripts/snow.sh start
sleep 25   # HD boot to the Finder
open_folder
echo "ICON_X_TUNE=${ICON_X_TUNE:-484} ICON_X_DIAG=${ICON_X_DIAG:-361}"
launch "${ICON_X_TUNE:-484}"; wait_quit MoofTCPTune
launch "${ICON_X_DIAG:-361}"; wait_quit MoofDiag
snow_stop
with_volume fetch_outs || die "outputs missing on the image (an app did not run: the launch click missed)"
echo "-- MoofTCPTune-$runid.out"; tr '\r' '\n' < "$out_tune"
echo "-- MoofDiag-$runid.out (tune:/stack:/RESULT lines)"; tr '\r' '\n' < "$out_diag" | grep -E '^(stack|tune|RESULT)'
if [ -n "$release" ]; then
  tr '\r' '\n' < "$out_tune" | grep -q '^RESULT FAIL not a tune build' || die "release: MoofTCPTune did not fail with 'not a tune build'"
  tr '\r' '\n' < "$out_diag" | grep -q '^tune:' && die "release: MoofDiag printed a tune: line"
  echo "release build refused the tune call as expected"; exit 0
fi
tr '\r' '\n' < "$out_tune" | grep -q '^RESULT OK$' || die "MoofTCPTune did not end RESULT OK"
tr '\r' '\n' < "$out_diag" | grep -q '^RESULT PASS$' || die "MoofDiag did not end RESULT PASS"
after="$(tr '\r' '\n' < "$out_tune" | sed -n 's/^after: //p')"
dtune="$(tr '\r' '\n' < "$out_diag" | sed -n 's/^tune: //p')"
[ -n "$after" ] && [ "$after" = "$dtune" ] || die "MoofDiag tune line '$dtune' differs from MoofTCPTune after '$after'"
echo "PASS: tune: $dtune"
