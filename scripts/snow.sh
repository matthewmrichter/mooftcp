#!/usr/bin/env bash
# Control Snow on a private Xvfb display. Usage:
#   snow.sh start [--floppy <img>]   render workspace, start Xvfb (if needed) and Snow
#   snow.sh stop                     kill Snow (and our Xvfb) by recorded PID
#   snow.sh status                   exit 0 running / 1 stopped
#   snow.sh shot <png>               screenshot the whole Xvfb screen
# MT_DISPLAY overrides the display (default :142). Never use :0/:1 (owner's desktop).
set -euo pipefail
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MT_DISPLAY="${MT_DISPLAY:-:142}"
SNOW_PID_FILE="$MT_HOME/snow.pid"
XVFB_PID_FILE="$MT_HOME/xvfb.pid"
WS="$MT_HOME/mooftcp.snoww"

die() { echo "snow.sh: $*" >&2; exit 1; }
case "$MT_DISPLAY" in :0|:1|:99) die "refusing display $MT_DISPLAY (not ours)";; esac

alive() { [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null; }
readpid() { if [ -f "$1" ]; then tr -d '[:space:]' < "$1"; fi; }

snow_running() {
  local p; p="$(readpid "$SNOW_PID_FILE")"
  alive "$p" && [ "$(cat "/proc/$p/comm" 2>/dev/null)" = snowemu ]
}

cmd_status() {
  if snow_running; then echo "running (pid $(readpid "$SNOW_PID_FILE"))"; exit 0; fi
  echo "stopped"; exit 1
}

cmd_start() {
  local floppy=()
  while [ $# -gt 0 ]; do
    case "$1" in
      --floppy)
        [ $# -ge 2 ] || die "--floppy needs an image"
        [ -f "$2" ] || die "no such floppy: $2"
        floppy=(--floppy "$2"); shift 2;;
      *) die "unknown start option: $1";;
    esac
  done
  if snow_running; then die "already running (pid $(readpid "$SNOW_PID_FILE"))"; fi
  if pgrep -x snowemu >/dev/null; then die "a snowemu we did not start is running; refusing"; fi
  [ -x "$MT_HOME/snow/snowemu" ] || die "missing $MT_HOME/snow/snowemu (run scripts/fetch-snow.sh)"
  [ -f "$MT_HOME/disks/work.hda" ] || die "missing work.hda (run scripts/fresh-disk.sh)"
  sed "s#@MT_HOME@#$MT_HOME#g" "$ROOT/tools/mooftcp.snoww.in" > "$WS"

  if ! xdpyinfo -display "$MT_DISPLAY" >/dev/null 2>&1; then
    Xvfb "$MT_DISPLAY" -screen 0 1280x1024x24 -nolisten tcp >/dev/null 2>&1 &
    echo $! > "$XVFB_PID_FILE"
    local ok=
    for _ in $(seq 50); do
      if xdpyinfo -display "$MT_DISPLAY" >/dev/null 2>&1; then ok=1; break; fi
      sleep 0.1
    done
    [ -n "$ok" ] || die "Xvfb $MT_DISPLAY did not come up"
  fi
  env -u WAYLAND_DISPLAY DISPLAY="$MT_DISPLAY" "$MT_HOME/snow/snowemu" "$WS" "${floppy[@]}" \
      >> "$MT_HOME/snow.log" 2>&1 &
  echo $! > "$SNOW_PID_FILE"
  sleep 1
  snow_running || die "Snow exited immediately; see $MT_HOME/snow.log"
  echo "started (pid $(readpid "$SNOW_PID_FILE"), display $MT_DISPLAY)"
}

cmd_stop() {
  local p; p="$(readpid "$SNOW_PID_FILE")"
  if snow_running; then
    kill "$p"
    for _ in $(seq 50); do alive "$p" || break; sleep 0.1; done
    if alive "$p"; then kill -9 "$p" 2>/dev/null || true; sleep 0.5; fi
  fi
  rm -f "$SNOW_PID_FILE"
  if pgrep -x snowemu >/dev/null; then die "snowemu still running after stop"; fi
  local x; x="$(readpid "$XVFB_PID_FILE")"
  if alive "$x" && [ "$(cat "/proc/$x/comm" 2>/dev/null)" = Xvfb ]; then
    kill "$x"
    for _ in $(seq 30); do alive "$x" || break; sleep 0.1; done
  fi
  rm -f "$XVFB_PID_FILE"
  echo "snow stopped"
}

cmd_shot() {
  [ $# -eq 1 ] || die "usage: snow.sh shot <png>"
  DISPLAY="$MT_DISPLAY" scrot -o "$1"
  echo "$1"
}

sub="${1:-}"
if [ $# -gt 0 ]; then shift; fi
case "$sub" in
  start) cmd_start "$@";;
  stop) cmd_stop;;
  status) cmd_status;;
  shot) cmd_shot "$@";;
  *) die "usage: snow.sh start [--floppy img] | stop | status | shot <png>";;
esac
