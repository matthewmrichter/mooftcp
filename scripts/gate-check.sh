#!/usr/bin/env bash
# Retro68 compile gate (Plan 3 Task 1; PLAN3-CARRYOVER 4.3 item H).
#   gate-check.sh
# 1. Every Mac target (mac/, which includes the core as mtcore68k) must build with
#    -m68000 -O2 -Wall -Wextra -Werror in a separate tree build-gate/ (build-mac/ is not touched).
#    Any warning or error fails the gate (exit 1). Log: build-gate.log.
# 2. Probe: core/*.c compiled alone with -mshort (16-bit int). Hazards (warnings) are listed;
#    this step is report-only and never changes the exit status.
# Exit status 0 only when step 1 passes.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
R68_TOOLCHAIN=/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
FLAGS="-m68000 -O2 -Wall -Wextra -Werror"
log="$ROOT/build-gate.log"

echo "== gate: all Mac targets, $FLAGS"
if ! scripts/r68 cmake -S mac -B build-gate -DCMAKE_TOOLCHAIN_FILE="$R68_TOOLCHAIN" \
    -DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_ASM_FLAGS=-m68000 >"$log" 2>&1; then
  tail -20 "$log"; echo "GATE FAIL (configure)"; exit 1
fi
if ! scripts/r68 cmake --build build-gate -- -k >>"$log" 2>&1; then
  grep -E "error|warning" "$log" | head -30
  echo "GATE FAIL (build; full log $log)"
  exit 1
fi
if grep -q "warning:" "$log"; then grep "warning:" "$log" | head -30; echo "GATE FAIL (warnings)"; exit 1; fi
echo "gate: PASS"

echo "== probe: core/*.c with -mshort (report only)"
n=0
for f in core/*.c; do
  out="$(scripts/r68 m68k-apple-macos-gcc -mshort -m68000 -std=c99 -O2 -Wall -Wextra -Icore -c "$f" -o /dev/null 2>&1)"
  rc=$?
  out="$(printf '%s\n' "$out" | grep -v '^Using multiversal')"
  [ "$rc" -eq 0 ] || out="$out
(compile failed)"
  if [ -n "$out" ]; then
    n=$((n+1)); echo "-- $f"; printf '%s\n' "$out" | grep -E "warning|error|failed" | head -8
  fi
done
echo "probe: $n of $(ls core/*.c | wc -l) core files report -mshort hazards (informational; the build uses 32-bit int)"
exit 0
