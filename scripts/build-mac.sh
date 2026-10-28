#!/bin/sh
# Build Classic Mac targets inside the Retro68 container. Usage: build-mac.sh [target]
# Retro68's add_application(Foo ...) makes target Foo (the 68k code) plus Foo_APPL
# (the MacBinary Foo.bin); for an application target we build both.
set -e
cd "$(dirname "$0")/.."
R68_TOOLCHAIN=/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake

if [ ! -f build-mac/CMakeCache.txt ]; then
  scripts/r68 cmake -S mac -B build-mac \
    -DCMAKE_TOOLCHAIN_FILE="$R68_TOOLCHAIN" \
    -DCMAKE_C_FLAGS=-m68000
fi
if [ -n "$1" ]; then
  if scripts/r68 cmake --build build-mac --target help | grep -q "\.\.\. $1_APPL\$"; then
    scripts/r68 cmake --build build-mac --target "$1_APPL"
  else
    scripts/r68 cmake --build build-mac --target "$1"
  fi
else
  scripts/r68 cmake --build build-mac
fi
