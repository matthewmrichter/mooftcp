#!/usr/bin/env bash
# ApiProbe launch dialog test in Snow (MoofTCP image), driven through the GUI with xdotool:
#   1. launch ApiProbe without an N token in ApiProbe.cfg (cfg = "B<base>" only): the dialog must appear
#      -> build-mac/probe/dialog-1-start.png
#   2. type a bad IP (300.1.1.1) + Return: the message must appear in the dialog -> dialog-2-badip.png (it stays open)
#   3. type 172.17.0.1, Tab, "6", Return: group 6 runs (default IP = quiet mode, no "# begin" lines) until the final alert
#   4. ApiProbe.out and ApiProbe.last are read back and checked
# Run as: flock /tmp/mooftcp-snow.lock scripts/probe-dialog-test.sh [--base N]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
die() { echo "probe-dialog-test: $*" >&2; exit 1; }
base=17100
[ "${1:-}" = --base ] && base="$2"
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
IMG="$MT_HOME/disks/work.hda"
DISPLAY_NUM="${MT_DISPLAY:-:142}"
out_dir="$ROOT/build-mac/probe"; mkdir -p "$out_dir"
bin="$ROOT/build-mac/apiprobe/ApiProbe.bin"

scripts/build-mac.sh ApiProbe >/dev/null
scripts/build-mac.sh MoofTCP >/dev/null
started=""
if ! (exec 3<>/dev/tcp/127.0.0.1/$((base + 7))) 2>/dev/null; then
  scripts/testnet.py --base "$base" > "$out_dir/dialog.testnet.log" 2>&1 &
  started=$!; sleep 1
fi
cleanup() { scripts/snow.sh stop >/dev/null 2>&1 || true; [ -z "$started" ] || kill "$started" 2>/dev/null || true; }
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
  part="$(hfs_part)"; vol="$(hmount "$IMG" "$part" | sed -n 's/^Volume name is "\(.*\)"$/\1/p')"
  [ -n "$vol" ] || die "hmount failed"
  "$@" || rc=$?
  humount "$vol:" >/dev/null 2>&1 || true
  return $rc
}
put_cfg() { printf 'B%s' "$base" > "$out_dir/dialog.cfg"; hcopy -t "$out_dir/dialog.cfg" ":MoofTCP Test:ApiProbe.cfg"; }
get_files() { hcopy -t ":MoofTCP Test:ApiProbe.out" "$out_dir/dialog.out"; hcopy -t ":MoofTCP Test:ApiProbe.last" "$out_dir/dialog.last"; }
click() {
  local x=$1 y=$2 w i
  w="$(xdotool search --name Snow | head -1)"
  xdotool windowfocus "$w" 2>/dev/null || true
  for i in 1 2 3; do xdotool mousemove $((x-20+i*5)) $((y-20+i*5)); sleep 0.1; done
  xdotool mousemove "$x" "$y"; sleep 0.2
  xdotool mousedown 1; sleep 0.05; xdotool mouseup 1
  if [ "${3:-}" = dbl ]; then sleep 0.12; xdotool mousedown 1; sleep 0.05; xdotool mouseup 1; fi
}
keys() { local w; w="$(xdotool search --name Snow | head -1)"; xdotool windowfocus "$w" 2>/dev/null || true; xdotool key --delay 150 "$@"; }
shot() { scripts/snow.sh shot "$1" >/dev/null 2>&1; echo "screenshot $1"; }

scripts/snow.sh stop >/dev/null 2>&1 || true
scripts/fresh-disk.sh
scripts/install-moof.sh
scripts/deploy.sh "$bin" --to ":MoofTCP Test:" >/dev/null
with_volume put_cfg
export DISPLAY="$DISPLAY_NUM"; unset WAYLAND_DISPLAY
scripts/snow.sh start
sleep 25
click 914 180 dbl; sleep 3
click 341 290 dbl; sleep 3
click 238 322 dbl; sleep 5
click 238 322 dbl; sleep 4
shot "$out_dir/dialog-1-start.png"
keys 3 0 0 period 1 period 1 period 1; sleep 1
keys Return; sleep 1
shot "$out_dir/dialog-2-badip.png"
keys 1 7 2 period 1 7 period 0 period 1; sleep 1
keys Tab; keys 6; sleep 1
shot "$out_dir/dialog-3-filled.png"
keys Return
t=0
while [ "$t" -lt 120 ]; do
  sleep 5; t=$((t+5))
  scripts/snow.sh shot "$out_dir/dialog-poll.png" >/dev/null 2>&1 || true
  # the final alert: wide white box (same test as probe-run.sh)
  if python3 -W ignore - "$out_dir/dialog-poll.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("L")
def white(box):
    px = list(im.crop(box).getdata()); return sum(1 for p in px if p > 230) / len(px)
sys.exit(0 if white((130, 200, 300, 212)) > 0.95 and white((560, 330, 700, 390)) > 0.6 else 1)
PY
  then break; fi
done
shot "$out_dir/dialog-4-end.png"
sleep 2
scripts/snow.sh stop >/dev/null 2>&1
with_volume get_files || die "ApiProbe.out / ApiProbe.last missing"
echo "--- ApiProbe.last:"; cat "$out_dir/dialog.last"; echo
echo "--- ApiProbe.out (head):"; head -8 "$out_dir/dialog.out" | cut -c1-120
grep -q '^# G6' "$out_dir/dialog.out" || die "group 6 did not run"
grep -q '^# begin' "$out_dir/dialog.out" && die "default IP must stay in the quiet mode"
grep -q '^172.17.0.1 6' "$out_dir/dialog.last" || die "ApiProbe.last content unexpected"
echo "PASS"
