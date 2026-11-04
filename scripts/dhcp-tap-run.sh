#!/usr/bin/env bash
# DHCP over TAP (Plan 4, docs/dev/EMULATOR.md "DHCP over TAP"): MoofTCP in DHCP mode gets its address
# from a real DHCP server (dnsmasq on tap0, started by the owner's `sudo scripts/tap-up.sh --dhcp`).
# Needs no root itself.
#
#   scripts/dhcp-tap-run.sh [--dev tap0]
#
# Runs scripts/moofdiag-run.sh --dhcp with MT_ETH_LINK=tap:<dev> (fresh work.hda, DHCP-mode prefs with
# fallbacks gateway 10.0.0.1 / DNS 1.1.1.1, diagnostic log on), then checks MoofDiag.out: the open
# succeeded (RESULT PASS), the client is BOUND (state 4) with an address from the pool 10.0.0.50-99, the
# view agrees, and the DHCP log records show INIT -> SELECTING -> REQUESTING -> BOUND. Prints the
# server's side from /run/mooftcp-dnsmasq.<dev>.log (DISCOVER / OFFER / REQUEST / ACK for our MAC).
# Exit 0 = all checks passed. Other Snow users on the machine: wrap it, e.g.
#   flock /tmp/mooftcp-snow.lock scripts/dhcp-tap-run.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dev="tap0"
die() { echo "dhcp-tap-run.sh: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
  case "$1" in
    --dev) [ $# -ge 2 ] || die "--dev needs a name"; dev="$2"; shift 2;;
    *) echo "usage: dhcp-tap-run.sh [--dev tap0]" >&2; exit 2;;
  esac
done
DNSM="/run/mooftcp-dnsmasq.$dev"
[ -e "/sys/class/net/$dev" ] || die "$dev does not exist: the owner runs 'sudo scripts/tap-up.sh --dhcp' first"
[ -f "$DNSM.pid" ] && kill -0 "$(cat "$DNSM.pid")" 2>/dev/null ||
  die "no DHCP server on $dev ($DNSM.pid): the owner runs 'sudo scripts/tap-up.sh --down' then 'sudo scripts/tap-up.sh --dhcp'"
logn=0; [ -r "$DNSM.log" ] && logn="$(wc -l < "$DNSM.log")"

cd "$ROOT"
MT_ETH_LINK="tap:$dev" scripts/moofdiag-run.sh --dhcp
out="$ROOT/build-mac/moofdiag/MoofDiag.out"
txt="$(tr '\r' '\n' < "$out")"
fail=0
check() {  # check <description> <extended regex>
  if printf '%s\n' "$txt" | grep -Eq "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi
}
check "open succeeded (RESULT PASS)" '^RESULT PASS$'
check "client BOUND with a pool address" '^dhcp: state 4 addr 10\.0\.0\.(5[0-9]|[6-9][0-9]) server 10\.0\.0\.1 '
check "view: DHCP mode, linked, state 4" '^view: magic 4d76 version 1 mode 1 state 4 linked 1 addr 0a0000'
check "lease applied: mask /24, gateway 10.0.0.1, DNS 1.1.1.1, domain moof.test" \
  "^core config: addr 10\.0\.0\.[5-9][0-9] mask 255\.255\.255\.0 gateway 10\.0\.0\.1 dns 1\.1\.1\.1 domain 'moof\.test'"
check "log: a DHCP BOUND record (state 4)" 'log: t [0-9]+ DHCP a 4 '
printf '%s\n' "$txt" | grep -E '^(dhcp|view|core config|log: t [0-9]+ DHCP)' || true
if [ -r "$DNSM.log" ]; then
  echo "== server side ($DNSM.log, new lines)"
  tail -n +"$((logn + 1))" "$DNSM.log" | grep -E 'DHCP(DISCOVER|OFFER|REQUEST|ACK|NAK)' || echo "(no DHCP lines)"
fi
[ "$fail" = 0 ] || die "checks failed (MoofDiag.out: $out)"
echo "dhcp-tap-run.sh: PASS"
