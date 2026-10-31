#!/usr/bin/env bash
# Keystroke-to-host latency over the TAP link (Task 13): Flynn -> scripts/latsrv.py on 10.0.0.1:17051.
#   lat-run.sh [--mactcp] [--keys N]
# --mactcp runs the golden image's MacTCP 2.1 instead of MoofTCP. Needs tap0 (owner: sudo scripts/tap-up.sh).
# Output: build-mac/lat/<stack>.txt (statistics) and <stack>.csv (inject->arrival ms per key).
set -euo pipefail
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export DISPLAY="${MT_DISPLAY:-:142}"; unset WAYLAND_DISPLAY
stack=moof; keys=60
while [ $# -gt 0 ]; do
  case "$1" in
    --mactcp) stack=mactcp; shift;;
    --keys) keys="$2"; shift 2;;
    *) echo "usage: lat-run.sh [--mactcp] [--keys N]" >&2; exit 2;;
  esac
done
cd "$ROOT"
out="$ROOT/build-mac/lat"; mkdir -p "$out"
flynn="${FLYNN_BIN:-$HOME/projects/flynn/build/Flynn.bin}"
cleanup() { scripts/snow.sh stop >/dev/null 2>&1 || true; }
trap cleanup EXIT
click() {
  local x=$1 y=$2 w i
  w="$(xdotool search --name Snow | head -1)"; xdotool windowfocus "$w" 2>/dev/null || true
  for i in 1 2 3; do xdotool mousemove $((x-20+i*5)) $((y-20+i*5)); sleep 0.1; done
  xdotool mousemove "$x" "$y"; sleep 0.2
  xdotool mousedown 1; sleep 0.05; xdotool mouseup 1
  if [ "${3:-}" = dbl ]; then sleep 0.12; xdotool mousedown 1; sleep 0.05; xdotool mouseup 1; fi
}
scripts/snow.sh stop >/dev/null 2>&1 || true
scripts/fresh-disk.sh >/dev/null
[ "$stack" = moof ] && scripts/install-moof.sh >/dev/null
scripts/deploy.sh "$flynn" --to ":MoofTCP Test:" >/dev/null
MT_ETH_LINK=tap:tap0 scripts/snow.sh start
sleep 28
click 914 180 dbl; sleep 3; click 341 290 dbl; sleep 3
click 238 325 dbl; sleep 8
click 570 247; xdotool type --delay 120 10.0.0.1
click 412 304 dbl; for i in $(seq 1 8); do xdotool key BackSpace; done; xdotool type --delay 120 17051
( sleep 3; click 719 519 ) &
python3 scripts/latsrv.py --keys "$keys" --out "$out/$stack.csv" | tee "$out/$stack.txt"
scripts/snow.sh shot "$out/$stack.png" >/dev/null 2>&1 || true
xdotool keydown Super_L; sleep 0.2; xdotool key q; sleep 0.2; xdotool keyup Super_L; sleep 3; xdotool key Return; sleep 2
