#!/usr/bin/env bash
# Snow tests of the application "MoofTCP Installer" (Plan 3 Task 10). Images are read and edited only
# with Snow stopped (hfsutils on $MT_HOME/disks/work.hda; golden.hda is never mounted).
#
#   installer-test.sh snap <label>        record the MacTCP trio (sha256 of the MacBinary from hcopy -m:
#                                         both forks, type, creator, Finder flags, location, folder field,
#                                         dates), the folder each one is in, the installer's own files
#                                         and the Disabled folder -> build-mac/installer-test/<label>.txt
#   installer-test.sh compare <a> <b>     the trio lines of two snapshots must be identical (exit 1 if not)
#   installer-test.sh prep <golden|absent|factory> [--debug SPEC] [--prefs]
#                                         fresh work.hda; absent: the trio deleted; factory: the factory
#                                         MacTCP only (probe-unconfig.py --mode factory); the release app (or,
#                                         with --debug, "Installer Dbg" and ":MoofTCP Installer Debug" = SPEC,
#                                         e.g. I4 / R2) into ":MoofTCP Test:"; --prefs: a "MoofTCP Prefs" for
#                                         Snow's NAT into the System Folder; snapshot "before"
#   installer-test.sh scenario <1|2|3|4|5a|5b|7>   run a scenario end to end (Install/Remove through the GUI
#                                         with xdotool, docs/dev/EMULATOR.md); screenshots and snapshots
#                                         go to build-mac/installer-test/
# Scenarios: 1 golden Install then Remove, byte-identical trio; 2 MacTCP absent; 3 Install twice, Remove
# twice; 4 interruption (debug build stops after N changes, Snow is killed, Remove / Install again after
# the reboot); 5a Install + restart with MoofTCP configured: parade icon, MoofDiag (moofdiag-run.sh
# --installed), Flynn echo; 5b factory MacTCP: Install, restart, Remove, then ApiProbe unconfigured must
# equal docs/dev/probe/mactcp-unconfigured.out; 7 a locked MacTCP file (Install refuses, nothing changes).
#   installer-test.sh interrupt <name> <I|R><N> <remove|install>   one interruption case of scenario 4
set -euo pipefail
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="$MT_HOME/disks/work.hda"
OUT="$ROOT/build-mac/installer-test"
APP="$ROOT/build-mac/installer/MoofTCP Installer.bin"
DBG="$ROOT/build-mac/installer/Installer Dbg.bin"
DISPLAY_NUM="${MT_DISPLAY:-:142}"
# same guard as snow.sh: only our private Xvfb displays :140-:149 (never the owner's desktop :0/:1)
case "$DISPLAY_NUM" in :14[0-9]) ;; *) echo "installer-test.sh: refusing display $DISPLAY_NUM (only :140-:149)" >&2; exit 1;; esac
TRIO=("MacTCP" "MacTCP DNR" "MacTCP Prep")
die() { echo "installer-test.sh: $*" >&2; exit 1; }
mkdir -p "$OUT"
cd "$ROOT"

snow_running() { scripts/snow.sh status >/dev/null 2>&1 || pgrep -x snowemu >/dev/null; }
need_stopped() { if snow_running; then die "Snow is running; stop it first"; fi; }

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
  need_stopped
  part="$(hfs_part)"; [ -n "$part" ] || die "no Apple_HFS partition in $IMG"
  vol="$(hmount "$IMG" "$part" | sed -n 's/^Volume name is "\(.*\)"$/\1/p')"
  [ -n "$vol" ] || die "hmount failed"
  "$@" || rc=$?
  humount "$vol:" >/dev/null 2>&1 || true
  return $rc
}
exists() { hls -1a "$1" 2>/dev/null | grep -Fxq -- "$2"; }

# mbinfo <file.bin>: one line describing a MacBinary file
mbinfo() {
  python3 -I -c '
import hashlib, struct, sys
d = open(sys.argv[1], "rb").read()
dl, rl = struct.unpack(">II", d[83:91])
ro = 128 + ((dl + 127) // 128) * 128
cr, md = struct.unpack(">II", d[91:99])
v, h, w = struct.unpack(">hhH", d[75:81])
r = bytearray(d[ro:ro + rl])
r[16:256] = bytes(len(r[16:256]))      # header bytes 16..255: reserved for the system (see check_installed)
print("type=%s creator=%s flags=0x%04x loc=%d,%d fldr=%d crdat=%08x mddat=%08x data=%d/%s rsrc=%d/%s rsrcx=%s mb=%s" % (
    d[65:69].decode("mac_roman"), d[69:73].decode("mac_roman"), (d[73] << 8) | d[101], v, h, w, cr, md,
    dl, hashlib.sha256(d[128:128 + dl]).hexdigest()[:16], rl, hashlib.sha256(d[ro:ro + rl]).hexdigest()[:16],
    hashlib.sha256(bytes(r)).hexdigest()[:16], hashlib.sha256(d).hexdigest()))
' "$1"
}

snap_body() {
  local label="$1" f folder tmp
  tmp="$(mktemp -d)"
  for f in "${TRIO[@]}"; do
    folder=""
    if exists ":System Folder:" "$f"; then folder=":System Folder:"
    elif exists ":System Folder:MacTCP (Disabled):" "$f"; then folder=":System Folder:MacTCP (Disabled):"; fi
    if [ -z "$folder" ]; then echo "trio $f ABSENT"; continue; fi
    hcopy -m "$folder$f" "$tmp/x.bin"
    echo "trio $f folder=$folder $(mbinfo "$tmp/x.bin")"
    rm -f "$tmp/x.bin"
  done
  for f in "MoofTCP" "MoofTCP.tmp" "MoofTCP Prefs"; do
    if exists ":System Folder:" "$f"; then
      hcopy -m ":System Folder:$f" "$tmp/x.bin"; echo "sys $f $(mbinfo "$tmp/x.bin")"; rm -f "$tmp/x.bin"
    fi
  done
  if exists ":System Folder:" "MacTCP (Disabled)"; then
    echo "disabled-folder present"
    hls -1a ":System Folder:MacTCP (Disabled):" | sed 's/^/disabled /'
    if exists ":System Folder:MacTCP (Disabled):" "MoofTCP Manifest"; then
      hcopy -r ":System Folder:MacTCP (Disabled):MoofTCP Manifest" "$tmp/m.txt"
      echo "manifest $(tr '\r\t' '|>' < "$tmp/m.txt")"
    fi
  else
    echo "disabled-folder absent"
  fi
  rm -rf "$tmp"
}
cmd_snap() {
  local label="$1"
  with_volume snap_body "$label" > "$OUT/$label.txt"
  echo "== snapshot $label"; cat "$OUT/$label.txt"
}
# compare <a> <b> [full|nofolder|attrs] [name]: the trio lines (or one file's) of two snapshots.
#   full      everything: folder and the whole MacBinary (sha256)
#   nofolder  everything but the folder (a parked copy against the restored one)
#   attrs     type, creator, Finder flags, location, folder field, creation date: what survives
#             the real MacTCP rewriting its own "MacTCP" and "MacTCP DNR" at every boot (resource fork
#             and modification date; scripts/installer-test.sh control run, Task 10 report)
trio_lines() {  # trio_lines <label> <mode> [name]
  grep '^trio ' "$OUT/$1.txt" | { if [ -n "${3:-}" ]; then grep -F "trio $3 " | grep -v -F "trio $3 DNR" | grep -v -F "trio $3 Prep" || true; else cat; fi; } |
  case "$2" in
    full) cat;;
    nofolder) sed -E 's/ folder=:System Folder:(MacTCP \(Disabled\):)?//';;
    attrs) sed -E 's/ folder=:System Folder:(MacTCP \(Disabled\):)?//; s/ mddat=[^ ]*//; s/ data=[^ ]*//; s/ rsrc=[^ ]*//; s/ rsrcx=[^ ]*//; s/ mb=[^ ]*//';;
  esac
}
cmd_compare() {
  local mode="${3:-full}"
  if diff <(trio_lines "$1" "$mode" "${4:-}") <(trio_lines "$2" "$mode" "${4:-}"); then
    echo "COMPARE $1 $2 ($mode${4:+, $4}): trio identical"
  else
    echo "COMPARE $1 $2 ($mode${4:+, $4}): DIFFERENT"; return 1
  fi
}

# where <label> <folder-regex>: every trio file present is in that folder
where() {
  if grep '^trio ' "$OUT/$1.txt" | grep -v ' ABSENT$' | grep -v -E " folder=$2 " >/dev/null; then
    echo "WHERE $1: not all in $2"; return 1
  fi
  echo "WHERE $1: trio in $2"
}
# The MoofTCP file on the image must be the build's: type, creator, Finder flags, and the resource fork
# (data and map) byte for byte. Header bytes 16..255 of a resource fork are reserved for the system, and
# System 6 fills them when the fork is first written (observed: the name, Finder info, creation date and
# fork length of MoofTCP.tmp at that moment); the Resource Manager never reads them, so they are masked.
check_installed() {
  local want got
  want="$(python3 -I -c '
import hashlib, struct, sys
d = open(sys.argv[1], "rb").read(); dl, rl = struct.unpack(">II", d[83:91]); ro = 128 + ((dl + 127) // 128) * 128
r = bytearray(d[ro:ro + rl]); r[16:256] = bytes(len(r[16:256]))
print("type=cdev creator=ztcp flags=0x2000 data=0 rsrc=%d rsrcx=%s" % (rl, hashlib.sha256(bytes(r)).hexdigest()[:16]))
' "$ROOT/build-mac/moof/MoofTCP.bin")"
  got="$(grep '^sys MoofTCP ' "$OUT/$1.txt" | python3 -I -c '
import re, sys
l = sys.stdin.read()
g = lambda k: (re.search(" " + k + "=([^ ]*)", l) or [None, "?"])[1]
print("type=%s creator=%s flags=%s data=%s rsrc=%s rsrcx=%s" % (g("type"), g("creator"), g("flags"),
      g("data").split("/")[0], g("rsrc").split("/")[0], g("rsrcx")))
')"
  if [ "$got" = "$want" ]; then echo "CHECK $1: MoofTCP installed as built ($got)"
  else echo "CHECK $1: MoofTCP WRONG: got '$got' want '$want'"; return 1; fi
}

mk_debug_file() {  # MacBinary TEXT file holding SPEC (the debug count)
  python3 -I -c '
import struct, sys
spec = sys.argv[2].encode()
name = b"MoofTCP Installer Debug"
h = bytearray(128)
h[1] = len(name); h[2:2 + len(name)] = name
h[65:69] = b"TEXT"; h[69:73] = b"ttxt"
h[83:87] = struct.pack(">I", len(spec))
h[122] = 129; h[123] = 129
c = 0
for b in bytes(h[:124]):
    c ^= b << 8
    for _ in range(8):
        c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
h[124], h[125] = c >> 8, c & 0xFF
open(sys.argv[1], "wb").write(bytes(h) + spec + bytes((128 - len(spec) % 128) % 128))
' "$1" "$2"
}
set_debug_body() {
  hdel ":MoofTCP Installer Debug" 2>/dev/null || true
  [ "$1" = none ] || hcopy -m "$OUT/debug.bin" ":"
}
set_debug() {  # set_debug <SPEC|none>
  [ "$1" = none ] || mk_debug_file "$OUT/debug.bin" "$1"
  with_volume set_debug_body "$1"
}
put_app_body() {  # one app in the test folder (the Finder puts it at 238,322)
  local f
  for f in "MoofTCP Installer" "Installer Dbg" "MoofDiag" "Flynn"; do hdel ":MoofTCP Test:$f" 2>/dev/null || true; done
  hcopy -m "$1" ":MoofTCP Test:"
}
put_app() { with_volume put_app_body "$1"; }
delete_trio_body() { local f; for f in "${TRIO[@]}"; do hdel ":System Folder:$f"; done; }

cmd_prep() {
  local kind="$1" dbg="" prefs=""; shift
  while [ $# -gt 0 ]; do
    case "$1" in
      --debug) dbg="$2"; shift 2;;
      --prefs) prefs=1; shift;;
      *) die "prep: unknown option $1";;
    esac
  done
  need_stopped
  [ -f "$APP" ] && [ -f "$DBG" ] || die "build first: scripts/build-mac.sh MoofInstaller; scripts/build-mac.sh MoofInstallerDbg"
  scripts/fresh-disk.sh >/dev/null
  case "$kind" in
    golden) ;;
    absent) with_volume delete_trio_body;;
    factory) python3 -I scripts/probe-unconfig.py "$IMG" --mode factory;;
    *) die "prep: golden, absent or factory";;
  esac
  if [ -n "$prefs" ]; then
    scripts/mkprefs.sh --ip 10.0.0.2 --mask 255.0.0.0 --gw 10.0.0.1 --dns 1.1.1.1 >/dev/null
    scripts/deploy.sh "$ROOT/build-mac/prefs/MoofTCP Prefs.bin" --to ":System Folder:" >/dev/null
  fi
  if [ -n "$dbg" ]; then put_app "$DBG"; set_debug "$dbg"; else put_app "$APP"; set_debug none; fi
  cmd_snap before >/dev/null
  echo "prep $kind${dbg:+ debug $dbg}${prefs:+ prefs}: done (snapshot before)"
}

# ---- GUI (Snow on Xvfb :142 only, scripts/snow.sh) ----
export DISPLAY="$DISPLAY_NUM"; unset WAYLAND_DISPLAY
click() {  # click x y [dbl]
  local x=$1 y=$2 w i
  w="$(xdotool search --name Snow | head -1)"
  xdotool windowfocus "$w" 2>/dev/null || true
  for i in 1 2 3; do xdotool mousemove $((x-20+i*5)) $((y-20+i*5)); sleep 0.1; done
  xdotool mousemove "$x" "$y"; sleep 0.2
  xdotool mousedown 1; sleep 0.05; xdotool mouseup 1
  if [ "${3:-}" = dbl ]; then sleep 0.12; xdotool mousedown 1; sleep 0.05; xdotool mouseup 1; fi
}
shot() { scrot -o "$OUT/$1.png" >/dev/null 2>&1 || true; }
# finder | alert (Restart now?) | app | crash
screen_state() {
  local f="$OUT/.state.png"
  scrot -o "$f" >/dev/null 2>&1 || { echo none; return; }
  python3 -W ignore - "$f" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("L")
def dark(box):
    px = list(im.crop(box).getdata())
    return sum(1 for p in px if p < 100) / len(px)
def white(box):
    px = list(im.crop(box).getdata())
    return sum(1 for p in px if p > 230) / len(px)
if dark((230, 88, 410, 110)) > 0.05:          # "View  Special": the Finder's menu bar
    print("finder")
elif dark((140, 174, 860, 178)) > 0.9:        # the alert frame: 4 dark rows (ALRT 129 at 54,56)
    print("alert")
elif white((330, 240, 690, 340)) > 0.35 and dark((80, 140, 920, 160)) < 0.05:
    print("crash")
else:
    print("app")
PY
}
wait_state() {  # wait_state <state> <seconds>
  local t=0 st
  while [ "$t" -lt "$2" ]; do
    st="$(screen_state)"
    [ "$st" = "$1" ] && return 0
    sleep 1; t=$((t+1))
  done
  echo "state '$st' after ${2}s, wanted '$1'"; return 1
}
boot() {
  scripts/snow.sh start >/dev/null
  sleep 25
}
stop() { scripts/snow.sh stop >/dev/null 2>&1 || true; }
launch() {  # the app in ":MoofTCP Test:" at 238,322; launch any = another app (Flynn: its menu bar looks like the Finder's)
  local n before=""
  [ "${1:-}" = any ] && before="$(scrot -o "$OUT/.before.png" >/dev/null 2>&1; echo 1)"
  for n in 1 2 3 4 5 6; do
    click 914 180 dbl; sleep 3
    click 341 290 dbl; sleep 3
    click 238 322 dbl; sleep 5
    [ "$(screen_state)" = app ] && return 0
    if [ -n "$before" ]; then sleep 5; return 0; fi
    [ "$(screen_state)" = finder ] || { echo "launch: unexpected screen $(screen_state)"; }
    click 238 322 dbl; sleep 5
    [ "$(screen_state)" = app ] && return 0
    echo "launch attempt $n did not start the app; retrying"
  done
  return 1
}
# press <install|remove> <shot-name> <restart|notnow|none>: click the button, wait, screenshot; answer the alert
press() {
  local x
  case "$1" in install) x=202;; remove) x=413;; *) die "press $1";; esac
  click "$x" 587
  sleep 8
  local st; st="$(screen_state)"
  shot "$2"
  case "$3" in
    none) [ "$st" = app ] || { echo "after $1: screen '$st', expected no Restart alert"; return 1; };;
    notnow) [ "$st" = alert ] || { echo "after $1: screen '$st', expected the Restart alert"; return 1; }
            click 577 310; sleep 2; shot "$2-msg";;
    restart) [ "$st" = alert ] || { echo "after $1: screen '$st', expected the Restart alert"; return 1; }
             click 769 310;;
    auto) if [ "$st" = alert ]; then click 577 310; sleep 2; shot "$2-msg"; echo "($1: Restart alert answered Not now)"
          else echo "($1: no Restart alert: nothing changed)"; fi;;
  esac
  echo "pressed $1 -> $2.png"
}
quit_app() { click 797 587; sleep 6; wait_state finder 20; }

result() { if [ "$1" -eq 0 ]; then echo "SCENARIO $2: PASS"; else echo "SCENARIO $2: FAIL"; fi; return "$1"; }

scenario1() {  # golden: Install then Remove; byte-identical trio
  local rc=0
  cmd_prep golden
  boot; launch || die "launch failed"
  press install s1-install notnow || rc=1
  quit_app || rc=1
  stop; cmd_snap s1-installed
  where s1-installed ':System Folder:MacTCP \(Disabled\):' || rc=1
  check_installed s1-installed || rc=1
  cmd_compare before s1-installed attrs || rc=1             # MacTCP rewrote MacTCP / MacTCP DNR at boot 1
  cmd_compare before s1-installed nofolder "MacTCP Prep" || rc=1
  boot; launch || die "launch failed"                      # boot 2: MoofTCP, MacTCP parked (not loaded)
  press remove s1-remove notnow || rc=1
  quit_app || rc=1
  stop; cmd_snap s1-after
  where s1-after ':System Folder:' || rc=1
  cmd_compare s1-installed s1-after nofolder || rc=1        # byte-identical: parked copy == restored file
  cmd_compare before s1-after attrs || rc=1
  cmd_compare before s1-after full "MacTCP Prep" || rc=1
  grep -q '^disabled-folder absent' "$OUT/s1-after.txt" || rc=1
  grep -q '^sys MoofTCP ' "$OUT/s1-after.txt" && rc=1
  result $rc 1
}
scenario2() {  # MacTCP absent
  local rc=0
  cmd_prep absent
  boot; launch || die "launch failed"
  press install s2-install notnow || rc=1
  quit_app || rc=1
  stop; cmd_snap s2-installed
  check_installed s2-installed || rc=1
  grep -q '^disabled-folder absent' "$OUT/s2-installed.txt" || rc=1
  boot; launch || die "launch failed"
  press remove s2-remove notnow || rc=1
  quit_app || rc=1
  stop; cmd_snap s2-after
  cmd_compare before s2-after full || rc=1
  grep -q '^sys MoofTCP ' "$OUT/s2-after.txt" && rc=1
  result $rc 2
}
scenario3() {  # Install twice (the second across a reboot), Remove twice
  local rc=0
  cmd_prep golden
  boot; launch || die "launch failed"
  press install s3-install1 notnow || rc=1
  press install s3-install2 none || rc=1                   # "already installed", same boot
  quit_app || rc=1
  stop; cmd_snap s3-installed >/dev/null
  boot; launch || die "launch failed"
  press install s3-install3 none || rc=1                   # and again with MoofTCP running
  press remove s3-remove1 notnow || rc=1
  press remove s3-remove2 none || rc=1                     # "not installed, nothing to remove"
  quit_app || rc=1
  stop; cmd_snap s3-after
  where s3-after ':System Folder:' || rc=1
  cmd_compare s3-installed s3-after nofolder || rc=1
  cmd_compare before s3-after attrs || rc=1
  result $rc 3
}

# one interruption: <name> <SPEC> <recovery: remove|install>
interrupt1() {
  local name="$1" spec="$2" how="$3" rc=0 what
  case "$spec" in R*) what=remove;; *) what=install;; esac
  cmd_prep golden --debug "$spec"
  boot; launch || die "launch failed"
  if [ "$what" = remove ]; then press install "$name-install" notnow || rc=1; fi
  press "$what" "$name-stop" none || rc=1
  stop                                       # power loss: Snow killed at the stop point
  cmd_snap "$name-crashed" >/dev/null
  put_app "$APP"; set_debug none
  boot
  shot "$name-reboot"
  launch || die "launch failed"
  if [ "$how" = install ]; then
    press install "$name-reinstall" notnow || rc=1
    quit_app || rc=1
    stop; cmd_snap "$name-reinstalled" >/dev/null
    check_installed "$name-reinstalled" || rc=1
    boot; launch || die "launch failed"
  fi
  press remove "$name-recover" auto || rc=1
  quit_app || rc=1
  stop; cmd_snap "$name-after" >/dev/null
  where "$name-after" ':System Folder:' || rc=1
  cmd_compare before "$name-after" attrs || rc=1
  cmd_compare before "$name-after" full "MacTCP Prep" || rc=1
  # a file that was parked when Snow was killed was not loaded again: byte-identical to that copy
  for f in "MacTCP" "MacTCP DNR"; do
    if trio_lines "$name-crashed" full "$f" | grep -q 'folder=:System Folder:MacTCP (Disabled):'; then
      cmd_compare "$name-crashed" "$name-after" nofolder "$f" || rc=1
    fi
  done
  if ! grep -q '^disabled-folder absent' "$OUT/$name-after.txt"; then
    # inst_plan.h: a crash after MKDIR but before the manifest exists leaves the folder with nothing to
    # vouch that the installer made it; Remove leaves an EMPTY one (harmless). Anything in it is a failure.
    if grep -q '^disabled ' "$OUT/$name-after.txt"; then echo "$name: Disabled folder left with files"; rc=1
    else echo "$name: an empty 'MacTCP (Disabled)' is left (no manifest was written before the stop)"; fi
  fi
  grep -q '^sys MoofTCP' "$OUT/$name-after.txt" && { echo "$name: MoofTCP file left"; rc=1; }
  echo "interrupt $name ($spec, then $how): $([ $rc -eq 0 ] && echo ok || echo FAILED)"
  return $rc
}
scenario4() {
  local rc=0
  interrupt1 s4-i1 I1 remove || rc=1        # after MKDIR
  interrupt1 s4-i4 I4 remove || rc=1        # manifest temp written, not renamed
  interrupt1 s4-i7 I7 remove || rc=1        # manifest done, first MacTCP file parked
  interrupt1 s4-i11 I11 remove || rc=1      # MoofTCP.tmp created and typed MTMP, no fork yet
  interrupt1 s4-i13 I13 remove || rc=1      # renamed to MoofTCP, still MTMP (incomplete)
  interrupt1 s4-i4b I4 install || rc=1
  interrupt1 s4-i13b I13 install || rc=1
  interrupt1 s4-r2 R2 remove || rc=1        # Remove: MoofTCP deleted, MacTCP DNR restored
  interrupt1 s4-r4 R4 remove || rc=1        # all three restored (MacTCP last), manifest not yet deleted
  result $rc 4
}
scenario5a() {  # boot test with MoofTCP configured (the prefs the user kept)
  local rc=0 i
  cmd_prep golden --prefs
  boot; launch || die "launch failed"
  press install s5a-install restart || rc=1
  # the restart: catch the parade icon (bottom left) while the INITs run
  for i in $(seq 1 120); do scrot -o "$OUT/s5a-boot-$(printf %03d "$i").png" >/dev/null 2>&1; sleep 0.25; done
  stop; cmd_snap s5a-installed >/dev/null
  check_installed s5a-installed || rc=1
  python3 -W ignore - "$OUT" <<'PY' || rc=1
import glob, sys
from PIL import Image
# the parade: a 32x32 icon at Mac (8, 342-40) = screen about (23..85, 657..719) on the grey desktop
# the icon's white face: about 70% white pixels in its box; the grey desktop dither and Snow's reset
# screen have under 10%
best = None
for f in sorted(glob.glob(sys.argv[1] + "/s5a-boot-*.png")):
    im = Image.open(f).convert("L").crop((32, 668, 75, 700))
    px = list(im.getdata())
    w = sum(1 for p in px if p > 200) / len(px)
    if best is None or w > best[0]:
        best = (w, f)
print("parade frame: %s (%.2f white in the icon box)" % (best[1], best[0]))
sys.exit(0 if best[0] > 0.5 else 1)
PY
  put_app_none() { hdel ":MoofTCP Test:MoofTCP Installer" 2>/dev/null || true; }
  with_volume put_app_none
  scripts/moofdiag-run.sh --installed || rc=1
  flynn_echo s5a || rc=1
  result $rc 5a
}
# Flynn to scripts/testnet.py's banner + echo port 17023 (ours, or another project's: shared, never killed)
flynn_echo() {
  local flynn="${FLYNN_BIN:-$HOME/projects/flynn/build/Flynn.bin}" tpid="" rc=0 w k
  [ -f "$flynn" ] || { echo "no $flynn"; return 1; }
  put_app "$flynn"
  if ! ss -ltn 2>/dev/null | grep -q ':17023 '; then
    python3 -I scripts/testnet.py > "$OUT/testnet.log" 2>&1 & tpid=$!
    sleep 1
  fi
  boot; launch any || { [ -z "$tpid" ] || kill "$tpid"; die "launch Flynn failed"; }
  shot "$1-flynn-start"
  w="$(xdotool search --name Snow | head -1)"
  click 570 247; sleep 1
  xdotool windowfocus "$w"; for k in 1 7 2 period 1 7 period 0 period 1; do xdotool key "$k"; sleep 0.3; done
  click 412 304 dbl; sleep 1
  xdotool windowfocus "$w"; for k in 1 7 0 2 3; do xdotool key "$k"; sleep 0.3; done
  click 719 519; sleep 8
  xdotool windowfocus "$w"; for k in m o o f space e c h o; do xdotool key "$k"; sleep 0.3; done
  sleep 3
  shot "$1-flynn"
  stop
  [ -z "$tpid" ] || kill "$tpid" 2>/dev/null || true
  if [ -n "$tpid" ]; then
    grep -q "recv" "$OUT/testnet.log" && echo "flynn: testnet.py saw the typed bytes" || { echo "flynn: no data at testnet.py"; rc=1; }
  else
    echo "flynn: shared testnet.py; check $1-flynn.png for the echo"
  fi
  return $rc
}
scenario5b() {  # factory MacTCP: Install, restart (MoofTCP unconfigured), Remove, then real MacTCP's probe
  local rc=0
  cmd_prep factory
  boot; launch || die "launch failed"
  press install s5b-install restart || rc=1
  sleep 40
  shot s5b-moof-booted
  launch || die "launch failed"
  press remove s5b-remove notnow || rc=1
  quit_app || rc=1
  stop; cmd_snap s5b-after >/dev/null
  where s5b-after ':System Folder:' || rc=1
  # the factory image has no "MacTCP DNR"/"MacTCP Prep": MacTCP makes them at boot 1, before Install parks them
  cmd_compare before s5b-after attrs "MacTCP" || rc=1
  put_app_none() { hdel ":MoofTCP Test:MoofTCP Installer" 2>/dev/null || true; }
  with_volume put_app_none
  scripts/probe-run.sh --unconfigured --keep-disk installer-5b || rc=1
  if diff "$ROOT/docs/dev/probe/mactcp-unconfigured.out" "$ROOT/build-mac/probe/installer-5b.out" >/dev/null; then
    echo "probe: identical to docs/dev/probe/mactcp-unconfigured.out"
  else
    diff "$ROOT/docs/dev/probe/mactcp-unconfigured.out" "$ROOT/build-mac/probe/installer-5b.out" | head -20
    rc=1
  fi
  result $rc 5b
}

scenario7() {  # a locked MacTCP file: Install refuses with the planner's message and changes nothing
  local rc=0
  cmd_prep golden
  lock_body() { hattrib +l ":System Folder:MacTCP DNR"; hls -l ":System Folder:" | grep "MacTCP DNR"; }
  with_volume lock_body
  cmd_snap before >/dev/null
  boot; launch || die "launch failed"
  press install s7-locked none || rc=1
  quit_app || rc=1
  stop; cmd_snap s7-after >/dev/null
  where s7-after ':System Folder:' || rc=1
  cmd_compare before s7-after nofolder "MacTCP Prep" || rc=1
  grep -q '^sys MoofTCP' "$OUT/s7-after.txt" && rc=1
  grep -q '^disabled-folder absent' "$OUT/s7-after.txt" || rc=1
  result $rc 7
}

case "${1:-}" in
  snap) [ $# -eq 2 ] || die "snap <label>"; cmd_snap "$2";;
  compare) [ $# -ge 3 ] && [ $# -le 5 ] || die "compare <a> <b> [full|nofolder|attrs] [name]"; shift; cmd_compare "$@";;
  prep) shift; cmd_prep "$@";;
  scenario)
    [ $# -eq 2 ] || die "scenario <1|2|3|4|5a|5b>"
    trap stop EXIT
    case "$2" in 1) scenario1;; 2) scenario2;; 3) scenario3;; 4) scenario4;; 5a) scenario5a;; 5b) scenario5b;; 7) scenario7;;
      *) die "unknown scenario $2";; esac;;
  interrupt) [ $# -eq 4 ] || die "interrupt <name> <I|R><N> <remove|install>"; trap stop EXIT; interrupt1 "$2" "$3" "$4";;
  *) sed -n '2,24p' "$0"; exit 2;;
esac
