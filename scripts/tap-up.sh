#!/usr/bin/env bash
# Owner-only (root): a TAP device for Snow's TapBridge Ethernet mode, so the host can reach the
# emulated Mac directly (inbound connections, passive opens, NCSA Telnet's FTP server). See
# docs/dev/EMULATOR.md "TAP mode".
#
#   sudo scripts/tap-up.sh [--dry-run] [--dev tap0] [--user NAME] [--no-nat]   # create
#   sudo scripts/tap-up.sh --down [--dry-run] [--dev tap0]                      # remove everything
#   scripts/tap-up.sh --dry-run ...                                            # print, no root needed
#
# Up: creates the persistent TAP device <dev> owned by <user> (default $SUDO_USER), gives the host
# 10.0.0.1/24 on it (the same gateway address as Snow's NAT, so the default "MoofTCP Prefs" and the
# golden MacTCP settings, 10.0.0.2/8 gateway 10.0.0.1, work unchanged), and, unless --no-nat,
# enables IPv4 forwarding plus a MASQUERADE rule so the guest still reaches the internet (DNS
# 1.1.1.1, the Wikipedia proxy). Every firewall rule carries the comment "mooftcp-tap" and is
# inserted at the top of FORWARD (Docker sets the FORWARD policy to DROP). The previous
# net.ipv4.ip_forward value is saved in /run/mooftcp-tap.<dev> and restored by --down.
# Only the 10.0.0.0/24 route is added, so other 10.x networks on the host are not shadowed.
# It also redirects guest -> 10.0.0.1 ports 21 (TCP) and 7 (TCP, UDP) to the unprivileged host test
# servers on 17021 (FTP) and 17007 (testnet.py echo); see redirects() below.
# Note: with NAT on, the guest can reach the owner's whole LAN, not only the internet.
# Rules are added only if absent (iptables -C) and removed wherever present, so re-running is safe.
# Only one MoofTCP TAP device at a time (shared subnet and saved ip_forward): a second is refused.
# Snow then runs with: MT_ETH_LINK=tap:<dev> scripts/snow.sh start
set -euo pipefail
dev="tap0"; user="${SUDO_USER:-${USER:-}}"; dry=""; down=""; nat=1
NET="10.0.0.0/24"; HOSTIP="10.0.0.1/24"; TAG="mooftcp-tap"
die() { echo "tap-up.sh: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run|-n) dry=1; shift;;
    --down) down=1; shift;;
    --dev) [ $# -ge 2 ] || die "--dev needs a name"; dev="$2"; shift 2;;
    --user) [ $# -ge 2 ] || die "--user needs a name"; user="$2"; shift 2;;
    --no-nat) nat=""; shift;;
    -h|--help) sed -n '2,23p' "$0"; exit 0;;
    *) die "unknown argument $1 (see --help)";;
  esac
done
[[ "$dev" =~ ^(tap|snow)[A-Za-z0-9_]{0,10}$ ]] || die "device name must start with tap or snow (Snow's rule)"
STATE="/run/mooftcp-tap.$dev"
run() { echo "+ $*"; [ -n "$dry" ] || "$@"; }
try() { echo "+ $* || true"; [ -n "$dry" ] || "$@" 2>/dev/null || true; }
if [ -z "$dry" ] && [ "$(id -u)" != 0 ]; then
  die "needs root: sudo $0 $* (or --dry-run to print the commands)"
fi
uplink() { ip -4 route show default 2>/dev/null | sed -n 's/.* dev \([^ ]*\).*/\1/p' | head -1; }

# Idempotent rule handling: add only when `iptables -C` does not find the rule (a second run or a
# half-finished earlier run never duplicates it); delete every copy that is present.
ipt_add() {  # ipt_add <table> <chain> <I|A> <rule...>
  local tbl="$1" chain="$2" how="$3"; shift 3
  if [ -z "$dry" ] && iptables -t "$tbl" -C "$chain" "$@" 2>/dev/null; then
    echo "= present: iptables -t $tbl $chain $*"; return 0
  fi
  if [ "$how" = I ]; then run iptables -t "$tbl" -I "$chain" 1 "$@"; else run iptables -t "$tbl" -A "$chain" "$@"; fi
}
ipt_del() {  # ipt_del <table> <chain> <rule...>
  local tbl="$1" chain="$2" n=0; shift 2
  if [ -n "$dry" ]; then echo "+ iptables -t $tbl -D $chain $* (while present)"; return 0; fi
  while [ $n -lt 8 ] && iptables -t "$tbl" -C "$chain" "$@" 2>/dev/null; do
    echo "+ iptables -t $tbl -D $chain $*"; iptables -t "$tbl" -D "$chain" "$@"; n=$((n+1))
  done
}

# The FORWARD rules let the guest reach anything the host can route to, including the owner's
# whole LAN (and the LAN can answer); only connections the guest starts are let back in.
fw_rules() {  # fw_rules <add|del>
  if [ "$1" = add ]; then
    ipt_add filter FORWARD I -i "$dev" -s "$NET" -m comment --comment "$TAG" -j ACCEPT
    ipt_add filter FORWARD I -o "$dev" -d "$NET" -m conntrack --ctstate RELATED,ESTABLISHED \
      -m comment --comment "$TAG" -j ACCEPT
    ipt_add nat POSTROUTING A -s "$NET" ! -o "$dev" -m comment --comment "$TAG" -j MASQUERADE
  else
    ipt_del filter FORWARD -i "$dev" -s "$NET" -m comment --comment "$TAG" -j ACCEPT
    ipt_del filter FORWARD -o "$dev" -d "$NET" -m conntrack --ctstate RELATED,ESTABLISHED \
      -m comment --comment "$TAG" -j ACCEPT
    ipt_del nat POSTROUTING -s "$NET" ! -o "$dev" -m comment --comment "$TAG" -j MASQUERADE
  fi
}

# Well-known ports the test apps cannot change, redirected to the unprivileged host test servers
# (no root needed to run those): guest -> 10.0.0.1:21 goes to the FTP server on 17021 (Fetch 2.1.2
# has no port field and no passive mode), guest -> 10.0.0.1:7 (TCP and UDP) to testnet.py's echo on
# 17007 (MacTCP Watcher's UDP/TCP tests always use port 7). REDIRECT delivers to 10.0.0.1 itself,
# so scripts/testftp.py binds 10.0.0.1.
redirects() {  # redirects <add|del>
  local r
  for r in "tcp 21 17021" "tcp 7 17007" "udp 7 17007"; do
    set -- $r
    if [ "$OP" = add ]; then
      ipt_add nat PREROUTING A -i "$dev" -d 10.0.0.1 -p "$1" --dport "$2" \
        -m comment --comment "$TAG" -j REDIRECT --to-ports "$3"
    else
      ipt_del nat PREROUTING -i "$dev" -d 10.0.0.1 -p "$1" --dport "$2" \
        -m comment --comment "$TAG" -j REDIRECT --to-ports "$3"
    fi
  done
}

if [ -n "$down" ]; then
  OP=del redirects
  fw_rules del
  if [ -f "$STATE" ] || [ -n "$dry" ]; then
    prev="$( [ -f "$STATE" ] && cat "$STATE" || echo 0)"
    run sysctl -q -w "net.ipv4.ip_forward=$prev"
    run rm -f "$STATE"
  fi
  try ip link set "$dev" down
  try ip tuntap del dev "$dev" mode tap
  echo "tap-up.sh: $dev removed"
  exit 0
fi

[ -n "$user" ] || die "no user to own $dev (use --user NAME)"
id "$user" >/dev/null 2>&1 || die "no such user $user"
if [ -z "$dry" ] && [ -e "/sys/class/net/$dev" ]; then die "$dev already exists (run --down first)"; fi
# One MoofTCP TAP device at a time: the subnet, the redirects and the saved ip_forward value are
# shared, so a second device would fight the first and --down of one would restore ip_forward
# under the other.
if [ -z "$dry" ]; then
  for f in /run/mooftcp-tap.*; do
    if [ -e "$f" ] && [ "$f" != "$STATE" ]; then
      die "another MoofTCP TAP device is up (${f#/run/mooftcp-tap.}); take it down first"
    fi
  done
  if ip -4 -o addr show | grep -q ' 10\.0\.0\.1/'; then
    die "10.0.0.1 is already on a host interface; refusing"
  fi
fi
run ip tuntap add dev "$dev" mode tap user "$user"
run ip addr add "$HOSTIP" dev "$dev"
run ip link set "$dev" up
OP=add redirects
if [ -n "$nat" ]; then
  if [ -z "$dry" ]; then sysctl -n net.ipv4.ip_forward > "$STATE"; fi
  echo "+ sysctl -n net.ipv4.ip_forward > $STATE"
  run sysctl -q -w net.ipv4.ip_forward=1
  fw_rules add
  echo "tap-up.sh: guest traffic to the internet leaves via $(uplink || echo '?') (MASQUERADE)"
fi
echo "tap-up.sh: $dev up, network 10.0.0.0/24, host 10.0.0.1 (guest 10.0.0.2, gateway 10.0.0.1; the guest prefs' /8 mask covers it)."
echo "tap-up.sh: start Snow with: MT_ETH_LINK=tap:$dev scripts/snow.sh start"
