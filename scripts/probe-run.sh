#!/usr/bin/env bash
# Build ApiProbe, run it in Snow against whichever stack the image loads, and collect
# ApiProbe.out into build-mac/probe/<label>.out.
#   probe-run.sh [--moof] [--unconfigured] [--keep-disk] [--groups "1 2 3"] <label>
#     --moof          run against MoofTCP instead of MacTCP: after fresh-disk.sh, scripts/install-moof.sh
#                     parks the MacTCP files and installs build-mac/moof/MacTCP.bin (built first) and a
#                     "MoofTCP Prefs" for 10.0.0.2/8 (with --unconfigured: no prefs file)
#     --unconfigured  after fresh-disk.sh, reinstall the factory MacTCP file on work.hda
#                     (scripts/probe-unconfig.py --mode factory) and boot Snow with the Ethernet
#                     link Down (MT_ETH_LINK=Down) so no address is obtainable; the app then
#                     runs its unconfigured suite
#     --keep-disk     do not run fresh-disk.sh (use the current work.hda as is)
#     --groups        case groups to run (default "1 2 3 4 5 6 7 8 9 0"; 0 = group 10, see apiprobe.c)
# One Snow boot per group: Snow's NAT stops accepting new guest TCP connections after
# about 16 (it never closes the host side), so the groups run in separate boots on the same
# work.hda and the per-boot outputs are concatenated in order. An unconfigured run needs a
# single boot (the app ignores the groups then).
# Uses snow.sh, deploy.sh, fresh-disk.sh (plus hfsutils to read the file back and to write
# ApiProbe.cfg). Starts scripts/testnet.py when nothing listens on 17007 and stops it again.
# Environment: PROBE_TIMEOUT seconds to wait per boot for the app to finish (default 900),
# PROBE_UNCONF_MODE passed to probe-unconfig.py as --mode.
# Exit status is nonzero if the app never finished or ApiProbe.out is missing/empty.
set -euo pipefail
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="$MT_HOME/disks/work.hda"
DISPLAY_NUM="${MT_DISPLAY:-:142}"
TIMEOUT="${PROBE_TIMEOUT:-900}"
die() { echo "probe-run.sh: $*" >&2; exit 1; }
usage() { echo 'usage: probe-run.sh [--moof] [--unconfigured] [--keep-disk] [--groups "1 2 3"] <label>' >&2; exit 2; }

unconf=""; keepdisk=""; label=""; groups="1 2 3 4 5 6 7 8 9 0"; moof=""
while [ $# -gt 0 ]; do
  case "$1" in
    --moof) moof=1; shift;;
    --unconfigured) unconf=1; shift;;
    --keep-disk) keepdisk=1; shift;;
    --groups) [ $# -ge 2 ] || usage; groups="$2"; shift 2;;
    -h|--help) usage;;
    -*) usage;;
    *) [ -z "$label" ] || usage; label="$1"; shift;;
  esac
done
[ -n "$label" ] || usage
case "$label" in *[!A-Za-z0-9._-]*) die "label may only contain A-Z a-z 0-9 . _ -";; esac
if [ -n "$unconf" ]; then
  groups="1"
  # Unconfigured = no address from any source: factory MacTCP 2.1 file, no Prep/DNR file, and
  # the Ethernet link Down (a RARP server would otherwise answer with 10.0.0.2).
  PROBE_UNCONF_MODE="${PROBE_UNCONF_MODE:-factory}"
  export MT_ETH_LINK="${MT_ETH_LINK:-Down}"
fi

cd "$ROOT"
out_dir="$ROOT/build-mac/probe"
mkdir -p "$out_dir"
out="$out_dir/$label.out"
rm -f "$out" "$out".g* "$out".partial*

echo "== build"
scripts/build-mac.sh ApiProbe >/dev/null
bin="$ROOT/build-mac/apiprobe/ApiProbe.bin"
[ -f "$bin" ] || die "build did not produce $bin"
if [ -n "$moof" ]; then
  scripts/build-mac.sh MoofTCP >/dev/null
  [ -f "$ROOT/build-mac/moof/MacTCP.bin" ] || die "build did not produce build-mac/moof/MacTCP.bin"
fi

started_testnet=""
if ! (exec 3<>/dev/tcp/127.0.0.1/17007) 2>/dev/null; then
  scripts/testnet.py > "$out_dir/$label.testnet.log" 2>&1 &
  started_testnet=$!
  sleep 1
fi
snow_stop() { scripts/snow.sh stop >/dev/null 2>&1 || true; }
cleanup() {
  snow_stop
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
# with_volume <cmd...>: mount work.hda (Snow stopped), run the command, unmount
with_volume() {
  local part vol rc=0
  part="$(hfs_part)"; [ -n "$part" ] || die "no Apple_HFS partition in $IMG"
  vol="$(hmount "$IMG" "$part" | sed -n 's/^Volume name is "\(.*\)"$/\1/p')"
  [ -n "$vol" ] || die "hmount failed"
  "$@" || rc=$?
  humount "$vol:" >/dev/null 2>&1 || true
  return $rc
}
write_cfg() {  # write_cfg "<groups>" : ApiProbe.cfg (digits only)
  local f="$out_dir/$label.cfg"
  printf '%s' "$1" | tr -d ' ' > "$f"
  hcopy -t "$f" ":MoofTCP Test:ApiProbe.cfg"
}
fetch_out() { hcopy -t ":MoofTCP Test:ApiProbe.out" "$1"; }
del_out() { hdel ":MoofTCP Test:ApiProbe.out" 2>/dev/null || true; }

click() {  # click x y [dbl]
  local x=$1 y=$2 w i
  w="$(xdotool search --name Snow | head -1)"
  xdotool windowfocus "$w" 2>/dev/null || true
  for i in 1 2 3; do xdotool mousemove $((x-20+i*5)) $((y-20+i*5)); sleep 0.1; done
  xdotool mousemove "$x" "$y"; sleep 0.2
  xdotool mousedown 1; sleep 0.05; xdotool mouseup 1
  if [ "${3:-}" = dbl ]; then sleep 0.12; xdotool mousedown 1; sleep 0.05; xdotool mouseup 1; fi
}
shot="$out_dir/$label.png"
# screen_state: prints
#   finder  the Finder menu bar is showing (the app is not running)
#   run     the app is running (blank menu bar), nothing special on screen
#   done    ApiProbe's final alert is up (wide white box)
#   crash   some other white dialog (the system error box) is up
screen_state() {
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
# launch_app: Finder windows from an earlier boot may be restored, so one pass of double
# clicks does not always reach the app; repeat until the Finder menu bar is gone.
launch_app() {
  local n
  for n in 1 2 3 4 5 6; do
    click 914 180 dbl; sleep 3     # open the MoofHD window
    click 341 290 dbl; sleep 3     # open the "MoofTCP Test" folder
    click 238 322 dbl; sleep 4     # launch ApiProbe (the first double click may only select)
    click 238 322 dbl; sleep 3
    [ "$(screen_state)" != finder ] && return 0
    echo "launch attempt $n did not start the app; retrying"
  done
  return 1
}

echo "== disk"
snow_stop
[ -n "$keepdisk" ] || scripts/fresh-disk.sh
if [ -n "$moof" ]; then
  if [ -n "$unconf" ]; then scripts/install-moof.sh --no-prefs; else scripts/install-moof.sh; fi
elif [ -n "$unconf" ]; then
  python3 -I scripts/probe-unconfig.py "$IMG" ${PROBE_UNCONF_MODE:+--mode "$PROBE_UNCONF_MODE"}
fi
scripts/deploy.sh "$bin" --to ":MoofTCP Test:" >/dev/null

export DISPLAY="$DISPLAY_NUM"; unset WAYLAND_DISPLAY
for g in $groups; do
  echo "== group $g"
  with_volume write_cfg "$g"
  with_volume del_out
  scripts/snow.sh start
  sleep 25   # HD boot to the Finder
  if ! launch_app; then
    scripts/snow.sh shot "$shot" >/dev/null 2>&1 || true
    snow_stop
    die "could not launch ApiProbe from the Finder (screenshot $shot)"
  fi
  t=0; done_flag=""
  while [ "$t" -lt "$TIMEOUT" ]; do
    sleep 5; t=$((t+5))
    scripts/snow.sh status >/dev/null 2>&1 || die "Snow died"
    st="$(screen_state)"
    if [ "$st" = done ]; then done_flag=1; break; fi
    if [ "$st" = crash ]; then done_flag=crash; break; fi
  done
  if [ -z "$done_flag" ]; then
    scripts/snow.sh shot "$shot" >/dev/null 2>&1 || true
    snow_stop
    with_volume fetch_out "$out.partial.g$g" || true
    die "ApiProbe group $g did not finish within ${TIMEOUT}s (screenshot $shot, partial $out.partial.g$g)"
  fi
  echo "group $g ${done_flag/1/finished} after about ${t}s"
  sleep 2
  snow_stop
  with_volume fetch_out "$out.g$g" || die "ApiProbe.out missing on the image (group $g)"
  [ -s "$out.g$g" ] || die "ApiProbe.out is empty (group $g)"
  if [ "$done_flag" = crash ]; then
    echo "# G$g CRASHED: a system error dialog was on the screen; the lines above are what was written first" >> "$out.g$g"
    cp "$shot" "$out_dir/$label.g$g.crash.png"
  fi
done
for g in $groups; do cat "$out.g$g"; done > "$out"
rm -f "$out".g*
[ -s "$out" ] || die "ApiProbe.out is empty"
echo "wrote $out ($(wc -l < "$out") lines)"
