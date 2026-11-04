#!/usr/bin/env bash
# KeyLat end to end in Snow (mac/keylat + scripts/keylatsrv.py): KeyLat connects to the host server, Snow gets
# N keystrokes through XTest, the host reports per-key latency.
#   keylat-run.sh [--mactcp] [--keys N] [--port P] [--explore] [--nosrv] <label>
# Default: MoofTCP installed on the fresh disk; --mactcp runs the golden image's MacTCP 2.1 instead. Snow's NAT
# link (guest sees the host as 172.17.0.1). --explore stops after opening the folder (screenshot, for icon positions).
# Output: build-mac/keylat/<label>/ (host report, host.txt, csv, KeyLat.out from the Mac, screenshots).
# Needs the Snow lock when other agents share the machine:  flock /tmp/mooftcp-snow.lock scripts/keylat-run.sh x
# NOTE: Snow's guest clock runs about 1.5x faster than wall time and its input path adds tens of ms; the
# numbers prove the pipeline, not the Mac's latency (that is the hardware run, docs/dev/HARDWARE.md "KeyLat").
set -euo pipefail
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="$MT_HOME/disks/work.hda"
die() { echo "keylat-run.sh: $*" >&2; exit 1; }
stack=moof; keys=30; port=17061; explore=""; nosrv=""; label=""
while [ $# -gt 0 ]; do
  case "$1" in
    --mactcp) stack=mactcp; shift;;
    --keys) keys="$2"; shift 2;;
    --port) port="$2"; shift 2;;
    --explore) explore=1; shift;;
    --nosrv) nosrv=1; shift;;
    -*) die "usage: keylat-run.sh [--mactcp] [--keys N] [--port P] [--explore] [--nosrv] <label>";;
    *) label="$1"; shift;;
  esac
done
[ -n "$label" ] || die "need a label"
case "$label" in *[!A-Za-z0-9._-]*) die "bad label";; esac
export DISPLAY="${MT_DISPLAY:-:142}"; unset WAYLAND_DISPLAY
cd "$ROOT"
out="$ROOT/build-mac/keylat/$label"
rm -rf "$out"; mkdir -p "$out"

echo "== build"
scripts/build-mac.sh KeyLat >/dev/null
[ "$stack" = moof ] && scripts/build-mac.sh MoofTCP >/dev/null

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
put_cfg() {
  printf '172.17.0.1 %s\r' "$port" > "$out/KeyLat.cfg"
  hdel ":MoofTCP Test:KeyLat.out" 2>/dev/null || true
  hdel ":MoofTCP Test:KeyLat.cfg" 2>/dev/null || true
  hcopy -t "$out/KeyLat.cfg" ":MoofTCP Test:KeyLat.cfg"
}
fetch() {
  hcopy -t ":MoofTCP Test:KeyLat.out" "$out/KeyLat.out" 2>/dev/null && tr '\r' '\n' < "$out/KeyLat.out" > "$out/KeyLat.out.txt"
  return 0
}

echo "== disk ($stack)"
scripts/snow.sh stop >/dev/null 2>&1 || true
scripts/fresh-disk.sh >/dev/null
[ "$stack" = moof ] && scripts/install-moof.sh >/dev/null
scripts/deploy.sh build-mac/keylat/KeyLat.bin --to ":MoofTCP Test:" >/dev/null
with_volume put_cfg

srv=""
cleanup() {
  scripts/snow.sh stop >/dev/null 2>&1 || true
  if [ -n "$srv" ]; then kill "$srv" 2>/dev/null || true; fi
}
trap cleanup EXIT
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

echo "== run"
scripts/snow.sh start >/dev/null
sleep 32
click 914 180 dbl; sleep 3          # MoofHD
click 341 290 dbl; sleep 4          # MoofTCP Test
shot 0-folder
if [ -n "$explore" ]; then echo "explore: see $out/0-folder.png"; exit 0; fi
if [ -n "$nosrv" ]; then
  click "${KEYLAT_ICON_X:-238}" "${KEYLAT_ICON_Y:-322}" dbl; sleep 6
  xdotool key Return; sleep 25        # the connect attempt runs into its timeout (15 s) and shows the alert
  shot 2-running
  echo "nosrv: see $out/2-running.png (expect the 'Could not connect' alert)"; exit 0
fi
if [ -z "$srv" ]; then
  python3 scripts/keylatsrv.py --port "$port" --out "$out/run" --inject "$keys" --display "$DISPLAY" \
    --idle-timeout 25 > "$out/host.txt" 2>&1 &
  srv=$!
fi
sleep 1
click "${KEYLAT_ICON_X:-238}" "${KEYLAT_ICON_Y:-322}" dbl; sleep 6
shot 1-setup
xdotool key Return                  # Connect (host and port come from KeyLat.cfg)
sleep 4
shot 2-running
# the server waits for 20 calibration pings (about 5 s), then types the keys; wait for it to finish the injection
for i in $(seq 1 120); do grep -q "injected" "$out/host.txt" 2>/dev/null && break; sleep 1; done
shot 3-typed
sleep 3
xdotool keydown Super_L; sleep 0.2; xdotool key q; sleep 0.2; xdotool keyup Super_L   # Cmd-Q
for i in $(seq 1 60); do kill -0 "$srv" 2>/dev/null || break; sleep 1; done   # the server ends when KeyLat closes
wait "$srv" 2>/dev/null || true; srv=""
shot 4-end
scripts/snow.sh stop >/dev/null
with_volume fetch
cat "$out/host.txt"
if [ -f "$out/KeyLat.out.txt" ]; then
  echo "== KeyLat.out (Mac): $(grep -c '^K ' "$out/KeyLat.out.txt") key lines, $(grep -c '^S ' "$out/KeyLat.out.txt") sync lines"; tail -n 2 "$out/KeyLat.out.txt"
  python3 scripts/keylatsrv.py --report "$out/run.host.txt" --mac "$out/KeyLat.out.txt" --out "$out/joined" > "$out/report-joined.txt"
  cat "$out/report-joined.txt"
else
  die "KeyLat.out was not written (see $out)"
fi
grep -q "^KeyLat report: $keys key packets" "$out/host.txt" || die "FAIL: host did not see $keys key packets (see $out)"
echo "PASS ($out)"
