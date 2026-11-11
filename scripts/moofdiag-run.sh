#!/usr/bin/env bash
# Build MoofDiag (mac/moofdiag) and MoofTCP, install MoofTCP on a fresh work.hda (scripts/install-moof.sh),
# boot Snow, run MoofDiag and collect build-mac/moofdiag/MoofDiag.out (Task 10).
#   moofdiag-run.sh [--keep-disk] [--no-install] [--installed] [--no-prefs] [--keep-running]
#     --keep-disk     do not run fresh-disk.sh
#     --no-install    do not run install-moof.sh (whatever stack the image has)
#     --installed     the image already has MoofTCP from the installer app ("MoofTCP Installer", Task 10):
#                     --keep-disk --no-install, and refuse unless ":System Folder:MoofTCP" is cdev/ztcp
#     --no-prefs      install MoofTCP without "MoofTCP Prefs" (unconfigured)
#     --log           install prefs with the diagnostic log flag (mkprefs.sh --log); MoofDiag then lists the log records
#     --dhcp          install DHCP-mode prefs (mkprefs.sh --dhcp --log --gw 10.0.0.1 --dns 1.1.1.1: fallbacks only). Snow's
#                     NAT has no DHCP server, so the open waits 10 s and fails -23004 (RESULT UNCONFIGURED, accepted);
#                     MoofDiag then reads the block through the status view and shows the client's state, DISCOVER
#                     count and DHCP log records. Over TAP with a host server (EMULATOR.md "DHCP over TAP") it binds.
#     --run-id <id>   unattended run: put the run file "Agent Run" = "!!<id>" beside MoofDiag (mac/common/runid.c);
#                     MoofDiag then writes MoofDiag-<id>.out, shows no closing alert and quits. The script checks the
#                     app quit by itself (the Finder is back, no alert seen) and fetches MoofDiag-<id>.out
#     --stale         (with --run-id) leave a 6000-byte filler file as the output file instead of deleting it:
#                     checks that MoofDiag truncates (SetEOF) and the result has no stale tail
#     --keep-running  leave Snow running after MoofDiag finished (for more GUI work); the output file
#                     is then not fetched
# MT_LITE=1 runs the MoofTCP Lite build (build-mac-lite/; --dhcp then shows Lite running the DHCP-mode prefs
# as a manual configuration: no address in them, so RESULT UNCONFIGURED at once, no 10 s wait).
# Same launch and screen checks as scripts/enetspike-run.sh. Exit status 0 when MoofDiag.out was fetched
# and ends with RESULT PASS (or RESULT UNCONFIGURED with --no-prefs).
set -euo pipefail
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="$MT_HOME/disks/work.hda"
DISPLAY_NUM="${MT_DISPLAY:-:142}"
TIMEOUT="${MOOFDIAG_TIMEOUT:-300}"
die() { echo "moofdiag-run.sh: $*" >&2; exit 1; }
. "$ROOT/scripts/variant.sh"      # MT_LITE=1: MoofTCP Lite and its MoofDiag, output in build-mac-lite/moofdiag/

stale=""; runid=""; keepdisk=""; install=1; prefs=""; keeprun=""; logp=""; installed=""; dhcp=""
while [ $# -gt 0 ]; do
  case "$1" in
    --keep-disk) keepdisk=1; shift;;
    --no-install) install=""; shift;;
    --installed) installed=1; install=""; keepdisk=1; shift;;
    --no-prefs) prefs="--no-prefs"; shift;;
    --keep-running) keeprun=1; shift;;
    --log) logp=1; shift;;
    --dhcp) dhcp=1; shift;;
    --stale) stale=1; shift;;
    --run-id) [ $# -ge 2 ] || { echo "--run-id needs an id" >&2; exit 2; }; runid="$2"; shift 2;;
    *) echo "usage: moofdiag-run.sh [--keep-disk] [--no-install] [--installed] [--no-prefs] [--log] [--dhcp] [--run-id <id> [--stale]] [--keep-running]" >&2; exit 2;;
  esac
done

cd "$ROOT"
out_dir="$ROOT/$BM/moofdiag"
echo "== build"
scripts/build-mac.sh MoofDiag >/dev/null
[ -z "$install" ] || scripts/build-mac.sh MoofTCP >/dev/null
bin="$out_dir/MoofDiag.bin"
[ -f "$bin" ] || die "build did not produce $bin"
outname="MoofDiag.out"; [ -z "$runid" ] || outname="MoofDiag-$runid.out"
out="$out_dir/$outname"
shot="$out_dir/MoofDiag.png"
rm -f "$out" "$shot"
snow_stop() { scripts/snow.sh stop >/dev/null 2>&1 || true; }
if [ -z "$keeprun" ]; then trap snow_stop EXIT; fi
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
fetch_out() { hcopy -t ":MoofTCP Test:$outname" "$1"; }
put_stale() { head -c 6000 /dev/zero | tr '\0' 'X' > "$1" && hcopy -t "$1" ":MoofTCP Test:$outname"; }
put_runfile() { printf '!!%s' "$runid" > "$1" && hcopy -t "$1" ":MoofTCP Test:Agent Run"; }
has_moof() { hls -l ":System Folder:" | grep -E "^f +cdev/ztcp .* MoofTCP$" >/dev/null; }
del_out() { hdel ":MoofTCP Test:$outname" 2>/dev/null || true; hdel ":MoofTCP Test:Agent Run" 2>/dev/null || true; }

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
  local n ix=238
  [ -z "$runid" ] || ix=361         # "Agent Run" sorts first, so MoofDiag is the second icon
  for n in 1 2 3 4 5 6; do
    click 914 180 dbl; sleep 3     # open the MoofHD window
    click 341 290 dbl; sleep 3     # open the "MoofTCP Test" folder
    click $ix 322 dbl; sleep 4     # launch MoofDiag (the first double click may only select)
    click $ix 322 dbl; sleep 3
    [ -z "$runid" ] || return 0    # unattended: the app may already have quit, so the Finder proves nothing
    [ "$(screen_state)" != finder ] && return 0
    echo "launch attempt $n did not start the app; retrying"
  done
  return 1
}

echo "== disk"
snow_stop
[ -n "$keepdisk" ] || scripts/fresh-disk.sh
pargs=()
[ -z "$prefs" ] || pargs=("$prefs")
if [ -n "$dhcp" ] && [ -z "$prefs" ]; then
  scripts/mkprefs.sh --dhcp --gw 10.0.0.1 --dns 1.1.1.1 --log >/dev/null
  pargs=(--prefs "$ROOT/build-mac/prefs/MoofTCP Prefs.bin")
elif [ -n "$logp" ] && [ -z "$prefs" ]; then
  scripts/mkprefs.sh --ip 10.0.0.2 --mask 255.0.0.0 --gw 10.0.0.1 --dns 1.1.1.1 --log >/dev/null
  pargs=(--prefs "$ROOT/build-mac/prefs/MoofTCP Prefs.bin")
fi
[ -z "$install" ] || scripts/install-moof.sh ${pargs[@]+"${pargs[@]}"}
if [ -n "$installed" ]; then with_volume has_moof || die "--installed: no cdev/ztcp MoofTCP in the System Folder"; fi
scripts/deploy.sh "$bin" --to ":MoofTCP Test:" >/dev/null
with_volume del_out
if [ -n "$runid" ]; then
  rf="$(mktemp)"; with_volume put_runfile "$rf" || die "could not write the run file"
  [ -z "$stale" ] || with_volume put_stale "$rf" || die "could not write the stale file"
  rm -f "$rf"
fi

export DISPLAY="$DISPLAY_NUM"; unset WAYLAND_DISPLAY
echo "== run"
scripts/snow.sh start
sleep 25   # HD boot to the Finder
if ! launch_app; then
  scripts/snow.sh shot "$shot" >/dev/null 2>&1 || true
  die "could not launch MoofDiag from the Finder (screenshot $shot)"
fi
t=0; st=""
if [ -n "$runid" ]; then
  # unattended: MoofDiag quits by itself, so the Finder comes back; an alert ("done") means the "!!" was ignored
  sleep 5; t=5; seen=0
  while [ "$t" -lt "$TIMEOUT" ]; do
    sleep 5; t=$((t+5))
    scripts/snow.sh status >/dev/null 2>&1 || die "Snow died"
    st="$(screen_state)"
    [ "$st" = done ] && die "MoofDiag showed its closing alert (the run file was not honoured)"
    [ "$st" = crash ] && { scripts/snow.sh shot "$shot" >/dev/null 2>&1 || true; die "a system error dialog was on the screen (screenshot $shot)"; }
    if [ "$st" = finder ]; then seen=$((seen+1)); else seen=0; fi
    [ "$seen" -ge 2 ] && break
  done
  [ "$seen" -ge 2 ] || die "MoofDiag did not quit within ${TIMEOUT}s (screenshot $shot)"
  echo "app quit unattended after about ${t}s"
  snow_stop
  with_volume fetch_out "$out" || die "$outname missing on the image"
  tr '\r' '\n' < "$out"
  tr '\r' '\n' < "$out" | grep -q '^RESULT \(PASS\|UNCONFIGURED\)$' || die "MoofDiag reported FAIL"
  echo "wrote $out"; exit 0
fi
while [ "$t" -lt "$TIMEOUT" ]; do
  sleep 5; t=$((t+5))
  scripts/snow.sh status >/dev/null 2>&1 || die "Snow died"
  st="$(screen_state)"
  [ "$st" = done ] || [ "$st" = crash ] && break
done
echo "app state '$st' after about ${t}s"
[ "$st" = crash ] && { scripts/snow.sh shot "$shot" >/dev/null 2>&1 || true; die "a system error dialog was on the screen (screenshot $shot)"; }
[ "$st" = done ] || die "MoofDiag did not finish within ${TIMEOUT}s (screenshot $shot)"
if [ -n "$keeprun" ]; then
  click 669 386; echo "Snow left running (MoofDiag.out is on the image)"; exit 0
fi
sleep 2
snow_stop
with_volume fetch_out "$out" || die "MoofDiag.out missing on the image"
tr '\r' '\n' < "$out"
tr '\r' '\n' < "$out" | grep -q '^RESULT \(PASS\|UNCONFIGURED\)$' || die "MoofDiag reported FAIL"
echo "wrote $out"
