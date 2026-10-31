#!/usr/bin/env bash
# One command for "is the tree healthy" (Plan 3 Task 1).
#   check-all.sh [--snow]
# 1. host build + ctest (ASan/UBSan)           -> build/
# 2. scripts/gate-check.sh                      Retro68 compile gate (-Werror) + -mshort probe
# With --snow (needs Snow, the golden image and the owner media; about 10 minutes):
# 3. scripts/mem-check.sh                       resident-memory cap rules 1-3 and the stack rule 4
# 4. scripts/moofdiag-run.sh                    MoofDiag RESULT PASS
# 5. scripts/probe-run.sh --moof --unconfigured ApiProbe unconfigured suite == docs/dev/probe/moof-unconfigured.out
# Prints one PASS/FAIL line per step and RESULT PASS only when every step passed; exit 0 then.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
snow=""
case "${1:-}" in
  "") ;;
  --snow) snow=1;;
  *) echo "usage: check-all.sh [--snow]" >&2; exit 2;;
esac

ok=1
step() {  # step <name> <command...>
  local name="$1"; shift
  echo "== $name"
  if "$@"; then echo "PASS $name"; else echo "FAIL $name"; ok=""; fi
}
host_tests() {
  cmake -S . -B build >/dev/null && cmake --build build >/dev/null || return 1
  ctest --test-dir build --output-on-failure > build/check-all-ctest.log 2>&1
  local rc=$?
  tail -3 build/check-all-ctest.log
  return $rc
}
unconf_probe() {   # MoofTCP unconfigured: same rows as the recorded run (timing values =@N ignored)
  local got=build-mac/probe/check-unconf.out ref=docs/dev/probe/moof-unconfigured.out
  scripts/probe-run.sh --moof --unconfigured check-unconf || return 1
  diff <(sed 's/=@[0-9-]*/=@N/g' "$ref") <(sed 's/=@[0-9-]*/=@N/g' "$got") >/dev/null ||
    { echo "$got differs from $ref:"; diff <(sed 's/=@[0-9-]*/=@N/g' "$ref") <(sed 's/=@[0-9-]*/=@N/g' "$got") | head -20; return 1; }
}

step "host tests"  host_tests
step "compile gate" scripts/gate-check.sh
if [ -n "$snow" ]; then
  step "mem-check (rules 1-4)" scripts/mem-check.sh
  step "MoofDiag" scripts/moofdiag-run.sh
  step "ApiProbe unconfigured" unconf_probe
fi
if [ -n "$ok" ]; then echo "RESULT PASS"; exit 0; fi
echo "RESULT FAIL"; exit 1
