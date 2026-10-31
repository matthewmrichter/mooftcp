#!/usr/bin/env bash
# Fetch the Plan 2 test apps (docs/dev/APPS.md) into $MT_HOME/media/apps/<app>/, verify each
# archive's SHA-256, unpack it and write the MacBinary .bin that scripts/deploy.sh takes.
# Idempotent: an archive that is present with the right hash is not downloaded again.
# Needs curl, unar (StuffIt), hexbin (macutils) and python3. Nothing goes into git.
# Flynn is not fetched: it is the owner's fork, ~/projects/flynn/build/Flynn.bin.
set -euo pipefail
MT_HOME="${MT_HOME:-$HOME/.local/share/mooftcp}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APPS="$MT_HOME/media/apps"
die() { echo "fetch-apps.sh: $*" >&2; exit 1; }
for t in curl unar hexbin python3; do command -v "$t" >/dev/null || die "missing $t"; done
DM="https://discmaster.textfiles.com/file"

# app-dir | archive name | URL | sha256 | path of the app inside the unpacked archive | .bin name
LIST=(
"wikipedia|wikipedia-1.1.sit|https://jcs.org/wikipedia/wikipedia-1.1.sit|14761cefe2552886476cdf018445d4454f355fca31636af5d55c52e7b28908da|wikipedia-1.1/Wikipedia|Wikipedia.bin"
"mactcp-watcher|mactcp-watcher-112.hqx|$DM/11952/ftp.swcp.com.zip/ftp.swcp.com/InternetTCP/mactcp-watcher-112.hqx|d1a82cd5bb5fbd5e60770e7f196931627b7ebbaa77db9227ffa90284cf229fdd|MacTCPWatcher-112/MacTCP Watcher|MacTCP Watcher.bin"
"ncsa-telnet|Telnet2.6.sit.hqx|$DM/11936/ftp.ncsa.uiuc.edu.zip/ftp.ncsa.uiuc.edu/Telnet/Telnet2.6/Telnet2.6.sit.hqx|e91c4dd0a550a0e189436e85639c99048fcae0d8a7996a43696a4779df448f59|Telnet2.6/NCSA Telnet 2.6|NCSA Telnet 2.6.bin"
"fetch|Fetch2.1.2.sea.hqx|$DM/11581/2014.03.ftp.eri.u-tokyo.ac.jp.zip/ftp.eri.u-tokyo.ac.jp/pub/MAC/network/Fetch2.1.2.sea.hqx|f5d9a49c6ec39a9f84a56b0dc66179720197d64ef84bdff81caa0d81dae2b181|Fetch 2.1.2 Folder/Fetch 2.1.2|Fetch 2.1.2.bin"
"eudora|eudora131.hqx|$DM/11941/2014.06.ftp.qualcomm.com.tar/ftp.qualcomm.com/eudora/eudoralight/mac/english/1.3system6/eudora131.hqx|07c30271f87bc45d7f1860582fc66201200c0d3b92db18db3afa5dd3bb068d60|Eudora 1.3.1|Eudora 1.3.1.bin"
"turbogopher|TurboGopher1.0.8b3.hqx|$DM/10583/Internet%20Tools.iso/infoserv/gopher/Macintosh-TurboGopher/old-versions/TurboGopher1.0.8b3.hqx|2ed8243f43367efdf3a136de5aaa1ac97d31a4fb5ada995552e58dd0964b5b77|-|TurboGopher 1.0.8b3.bin"
"wallops|wallops-2.2.sit|https://jcs.org/wallops/wallops-2.2.sit|adc2b473664ec0960b5b028335566831dd69f4a1de40f79e22bf1337292cca88|-|-"
)

for row in "${LIST[@]}"; do
  IFS='|' read -r app arc url sum inner bin <<< "$row"
  d="$APPS/$app"; mkdir -p "$d"
  if [ ! -f "$d/$arc" ] || [ "$(sha256sum "$d/$arc" | cut -d' ' -f1)" != "$sum" ]; then
    curl -sfL -m 300 -o "$d/$arc.part" "$url" || die "download failed: $url"
    mv "$d/$arc.part" "$d/$arc"
  fi
  [ "$(sha256sum "$d/$arc" | cut -d' ' -f1)" = "$sum" ] || die "$app: SHA-256 mismatch for $arc"
  [ "$bin" != "-" ] || { echo "ok  $app ($arc, source only)"; continue; }
  tmp="$(mktemp -d)"
  src="$d/$arc"
  case "$arc" in
    *.hqx) (cd "$tmp" && hexbin "$src" >/dev/null 2>&1) || die "$app: hexbin failed"
           src="$(ls "$tmp"/*.bin | head -1)";;
  esac
  if [ "$inner" = "-" ]; then                 # the BinHex file holds the application itself; hexbin's
    python3 -I "$ROOT/scripts/ad2bin.py" --rewrap "$src" "$d/$bin" >/dev/null   # MacBinary needs re-encoding for hcopy
  else
    mkdir "$tmp/u"
    unar -q -k visible -o "$tmp/u" "$src" >/dev/null || die "$app: unar failed"
    python3 -I "$ROOT/scripts/ad2bin.py" "$tmp/u/$inner" "$d/$bin" >/dev/null
  fi
  rm -rf "$tmp"
  echo "ok  $app -> $d/$bin ($(sha256sum "$d/$bin" | cut -c1-16)...)"
done
