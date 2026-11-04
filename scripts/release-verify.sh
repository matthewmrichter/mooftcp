#!/usr/bin/env bash
# Verify the release artifacts with readers that did not write them (Plan 3 Task 11).
#   release-verify.sh [--self-test] <version>      e.g. release-verify.sh 1.0rc1
# Reads dist/MoofTCP-<version>.hqx  (macutils hexbin, and unar),
#       dist/MoofTCP-<version>.sit  (unar),
#       dist/MoofTCP-<version>.dsk  (hfsutils hmount/hls/hcopy -m)
# and compares names, type/creator, Finder flags, fork lengths and sha256 of both forks with
# build-mac/installer/MoofTCP Installer.bin and the Read Me derived from docs/release/ReadMe.txt.
# Also checks both MoofTCP files inside the installer (MTCP 128 = build-mac/, 129 = build-mac-lite/, the
# variant and version strings). Details: scripts/relverify.py. Exit 0 only if every format passes.
# --self-test: the real artifacts must pass, and a corrupted copy of each format (a resource-fork
# byte; the type code in the .dsk) must FAIL, otherwise exit 1.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 -I "$ROOT/scripts/relverify.py" "$@"
