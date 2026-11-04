cmake_minimum_required(VERSION 3.9)
# ASM: .s files (GNU as syntax) go through m68k-apple-macos-gcc; build-mac.sh passes -m68000
# in CMAKE_ASM_FLAGS so 68020+ instructions are rejected.
project(MoofTCPMac C ASM)

# MoofTCP Lite: the whole tree without the DHCP client (core/mt_dhcp.c left out, MT_NO_DHCP for every
# C target; Rez files get -DMT_NO_DHCP through MT_REZ_DEFS). scripts/build-mac.sh sets it with
# MT_LITE=1 and builds into build-mac-lite/, never into build-mac/.
option(MT_NO_DHCP "Build MoofTCP Lite (no DHCP client)" OFF)
set(MT_REZ_DEFS)
if(MT_NO_DHCP)
  add_compile_definitions(MT_NO_DHCP)
  set(MT_REZ_DEFS -DMT_NO_DHCP)
endif()

add_subdirectory(hello)
add_subdirectory(hello_init)
add_subdirectory(heapinfo)
add_subdirectory(apiprobe)
add_subdirectory(glue)
add_subdirectory(asmtest)
add_subdirectory(enetspike)
add_subdirectory(dnr)
add_subdirectory(cdev)
add_subdirectory(moof)
add_subdirectory(installer)   # after moof: it embeds build-mac/moof/MoofTCP.bin
add_subdirectory(moofdiag)
add_subdirectory(mfecho)
add_subdirectory(keylat)
