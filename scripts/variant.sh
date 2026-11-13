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
#                       so MT_TUNE=1 builds never mix with the release objects. The installer can be built there too
#                       (a perf kit's: MT_TUNE=1 build-mac.sh MoofInstaller, after configuring build-mac-tune/ with
#                       -DMT_LITE_MOOF_BIN=/src/build-mac-lite-tune/moof/MoofTCP.bin; moofagent sessions/kit-perf1.sh),
#                       but build-mac.sh does not build the Lite file first for it as it does for build-mac/.
#                       scripts/release.sh refuses MT_TUNE, MT_TRACE and MT_EXTRA_CFLAGS: releases come from build-mac/.
case "${MT_TUNE:-0}" in
  0) ;;
  1) BM="$BM-tune"; MT_EXTRA_CFLAGS="${MT_EXTRA_CFLAGS:-} -DMT_TUNE=1"; export MT_EXTRA_CFLAGS;;
  *) echo "MT_TUNE must be 0 or 1 (got '$MT_TUNE')" >&2; exit 2;;
esac
#   MT_TRACE=1          (only with MT_TUNE=1) the echo-path trace (compile option MT_TRACE, mac/glue/mt_trace.h: Control
#                       0x4D56, MoofDiag's TR lines, scripts/trace-echo.py; never a release): its own tree,
#                       build-mac-tune-trace/ (build-mac-lite-tune-trace/ with MT_LITE=1).
case "${MT_TRACE:-0}" in
  0) ;;
  1) [ "${MT_TUNE:-0}" = 1 ] || { echo "MT_TRACE=1 needs MT_TUNE=1 (perf builds only)" >&2; exit 2; }
     BM="$BM-trace"; MT_EXTRA_CFLAGS="${MT_EXTRA_CFLAGS:-} -DMT_TRACE=1"; export MT_EXTRA_CFLAGS;;
  *) echo "MT_TRACE must be 0 or 1 (got '$MT_TRACE')" >&2; exit 2;;
esac
