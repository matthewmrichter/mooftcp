#!/usr/bin/env bash
# Build AsmTest (68000 asm checksum/copy equivalence + timing), run it in Snow on a fresh
# work.hda and collect AsmTest.out into build-mac/asmtest/AsmTest.out.
#   asmtest-run.sh [--keep-disk]
#     --keep-disk  do not run fresh-disk.sh (use the current work.hda as is)
# Same launch and screen checks as scripts/probe-run.sh (the app sits in the first icon slot
# of ":MoofTCP Test:", and its final alert is ApiProbe's box). A system error box (an address
# error from a misaligned word/long access in the asm) is reported as a crash.
# Environment: ASMTEST_TIMEOUT seconds to wait for the app (default 900).
# Exit status: 0 only if AsmTest.out was fetched and ends with "RESULT PASS".
set -euo pipefail
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="$MT_HOME/disks/work.hda"
DISPLAY_NUM="${MT_DISPLAY:-:142}"
TIMEOUT="${ASMTEST_TIMEOUT:-900}"
die() { echo "asmtest-run.sh: $*" >&2; exit 1; }

keepdisk=""
case "${1:-}" in
  --keep-disk) keepdisk=1;;
  "") ;;
  *) echo "usage: asmtest-run.sh [--keep-disk]" >&2; exit 2;;
esac

cd "$ROOT"
out_dir="$ROOT/build-mac/asmtest"
echo "== build"
scripts/build-mac.sh AsmTest >/dev/null
bin="$out_dir/AsmTest.bin"
[ -f "$bin" ] || die "build did not produce $bin"
out="$out_dir/AsmTest.out"
shot="$out_dir/AsmTest.png"
rm -f "$out" "$shot"

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
fetch_out() { hcopy -t ":MoofTCP Test:AsmTest.out" "$1"; }
del_out() { hdel ":MoofTCP Test:AsmTest.out" 2>/dev/null || true; }

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
launch_app() {
  local n
  for n in 1 2 3 4 5 6; do
    click 914 180 dbl; sleep 3     # open the MoofHD window
    click 341 290 dbl; sleep 3     # open the "MoofTCP Test" folder
    click 238 322 dbl; sleep 4     # launch AsmTest (the first double click may only select)
    click 238 322 dbl; sleep 3
    [ "$(screen_state)" != finder ] && return 0
    echo "launch attempt $n did not start the app; retrying"
  done
  return 1
}

echo "== disk"
snow_stop
[ -n "$keepdisk" ] || scripts/fresh-disk.sh
scripts/deploy.sh "$bin" --to ":MoofTCP Test:" >/dev/null
with_volume del_out

export DISPLAY="$DISPLAY_NUM"; unset WAYLAND_DISPLAY
echo "== run"
scripts/snow.sh start
sleep 25   # HD boot to the Finder
if ! launch_app; then
  scripts/snow.sh shot "$shot" >/dev/null 2>&1 || true
  die "could not launch AsmTest from the Finder (screenshot $shot)"
fi
t=0; st=""
while [ "$t" -lt "$TIMEOUT" ]; do
  sleep 5; t=$((t+5))
  scripts/snow.sh status >/dev/null 2>&1 || die "Snow died"
  st="$(screen_state)"
  [ "$st" = done ] || [ "$st" = crash ] && break
done
echo "app state '$st' after about ${t}s"
sleep 2
snow_stop
with_volume fetch_out "$out" || die "AsmTest.out missing on the image (screenshot $shot)"
cat "$out" | tr '\r' '\n'
[ "$st" = crash ] && die "a system error dialog was on the screen (screenshot $shot)"
[ "$st" = done ] || die "AsmTest did not finish within ${TIMEOUT}s (screenshot $shot)"
tr '\r' '\n' < "$out" | grep -q '^RESULT PASS$' || die "AsmTest reported failures"
echo "wrote $out"
