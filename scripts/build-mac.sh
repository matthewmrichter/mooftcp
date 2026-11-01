#!/bin/sh
# Build Classic Mac targets inside the Retro68 container. Usage: build-mac.sh [target]
# Retro68's add_application(Foo ...) makes target Foo (the 68k code) plus Foo_APPL
# (the MacBinary Foo.bin); for an application target we build both.
# Build order: the installer (target MoofInstaller -> build-mac/installer/MoofTCP Installer.bin) embeds
# build-mac/moof/MoofTCP.bin, and CMake builds MoofTCP first (mac/installer/CMakeLists.txt), so
# `build-mac.sh MoofInstaller` alone is enough.
set -e
cd "$(dirname "$0")/.."
R68_TOOLCHAIN=/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake

# Configure on every call (cheap): flag changes here and new mac/ subdirectories apply
# without deleting build-mac/. MT_EXTRA_CFLAGS adds C flags (e.g. -DMT_STACK_PAINT=4096, mt_stackmark.h). -m68000 (C and asm): the target is a 68000 Mac Classic.
scripts/r68 cmake -S mac -B build-mac \
  -DCMAKE_TOOLCHAIN_FILE="$R68_TOOLCHAIN" \
  "-DCMAKE_C_FLAGS=-m68000 $MT_EXTRA_CFLAGS" -DCMAKE_ASM_FLAGS=-m68000 >/dev/null
if [ -n "$1" ]; then
  if scripts/r68 cmake --build build-mac --target help | grep -q "\.\.\. $1_APPL\$"; then
    scripts/r68 cmake --build build-mac --target "$1_APPL"
  else
    scripts/r68 cmake --build build-mac --target "$1"
  fi
else
  scripts/r68 cmake --build build-mac
fi
