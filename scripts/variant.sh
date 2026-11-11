# Sourced by the build and Snow scripts: the build variant (docs/dev/RELEASE.md "Variants").
#   MT_LITE unset or 0  the full build (DHCP client), tree build-mac/
#   MT_LITE=1           MoofTCP Lite (compile option MT_NO_DHCP: no DHCP client), tree build-mac-lite/
# Sets BM (the tree, relative to the repo root) and MT_NO_DHCP_OPT (the CMake switch value).
case "${MT_LITE:-0}" in
  0) BM=build-mac; MT_NO_DHCP_OPT=OFF;;
  1) BM=build-mac-lite; MT_NO_DHCP_OPT=ON;;
  *) echo "MT_LITE must be 0 or 1 (got '$MT_LITE')" >&2; exit 2;;
esac
#   MT_TUNE=1           the perf configuration (compile option MT_TUNE: run-time TCP timing levers, Control 0x4D55,
#                       MoofTCPTune; never a release): its own tree, build-mac-tune/ (build-mac-lite-tune/ with MT_LITE=1),
#                       so MT_TUNE=1 builds never mix with the release objects. The installer is not built there.
case "${MT_TUNE:-0}" in
  0) ;;
  1) BM="$BM-tune"; MT_EXTRA_CFLAGS="${MT_EXTRA_CFLAGS:-} -DMT_TUNE=1"; export MT_EXTRA_CFLAGS;;
  *) echo "MT_TUNE must be 0 or 1 (got '$MT_TUNE')" >&2; exit 2;;
esac
