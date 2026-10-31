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
    -h|--help) sed -n '2,20p' "$0"; exit 0;;
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

fw_rules() {  # fw_rules <-I|-D>: the three rules, inserted (-I) or deleted (-D)
  local op="$1"
  if [ "$op" = -I ]; then
    run iptables -I FORWARD 1 -i "$dev" -s "$NET" -m comment --comment "$TAG" -j ACCEPT
    run iptables -I FORWARD 1 -o "$dev" -d "$NET" -m conntrack --ctstate RELATED,ESTABLISHED \
      -m comment --comment "$TAG" -j ACCEPT
    run iptables -t nat -A POSTROUTING -s "$NET" ! -o "$dev" -m comment --comment "$TAG" -j MASQUERADE
  else
    try iptables -D FORWARD -i "$dev" -s "$NET" -m comment --comment "$TAG" -j ACCEPT
    try iptables -D FORWARD -o "$dev" -d "$NET" -m conntrack --ctstate RELATED,ESTABLISHED \
      -m comment --comment "$TAG" -j ACCEPT
    try iptables -t nat -D POSTROUTING -s "$NET" ! -o "$dev" -m comment --comment "$TAG" -j MASQUERADE
  fi
}

# Well-known ports the test apps cannot change, redirected to the unprivileged host test servers
# (no root needed to run those): guest -> 10.0.0.1:21 goes to the FTP server on 17021 (Fetch 2.1.2
# has no port field and no passive mode), guest -> 10.0.0.1:7 (TCP and UDP) to testnet.py's echo on
# 17007 (MacTCP Watcher's UDP/TCP tests always use port 7).
redirects() {  # redirects <-A|-D>
  local op="$1" f=run; [ "$op" = -D ] && f=try
  $f iptables -t nat "$op" PREROUTING -i "$dev" -d 10.0.0.1 -p tcp --dport 21 \
    -m comment --comment "$TAG" -j REDIRECT --to-ports 17021
  $f iptables -t nat "$op" PREROUTING -i "$dev" -d 10.0.0.1 -p tcp --dport 7 \
    -m comment --comment "$TAG" -j REDIRECT --to-ports 17007
  $f iptables -t nat "$op" PREROUTING -i "$dev" -d 10.0.0.1 -p udp --dport 7 \
    -m comment --comment "$TAG" -j REDIRECT --to-ports 17007
}

if [ -n "$down" ]; then
  redirects -D
  fw_rules -D
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
run ip tuntap add dev "$dev" mode tap user "$user"
run ip addr add "$HOSTIP" dev "$dev"
run ip link set "$dev" up
redirects -A
if [ -n "$nat" ]; then
  if [ -z "$dry" ]; then sysctl -n net.ipv4.ip_forward > "$STATE"; fi
  echo "+ sysctl -n net.ipv4.ip_forward > $STATE"
  run sysctl -q -w net.ipv4.ip_forward=1
  fw_rules -I
  echo "tap-up.sh: guest traffic to the internet leaves via $(uplink || echo '?') (MASQUERADE)"
fi
echo "tap-up.sh: $dev up, host 10.0.0.1 (guest 10.0.0.2/8, gateway 10.0.0.1)."
echo "tap-up.sh: start Snow with: MT_ETH_LINK=tap:$dev scripts/snow.sh start"
