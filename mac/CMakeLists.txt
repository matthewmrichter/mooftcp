cmake_minimum_required(VERSION 3.9)
# ASM: .s files (GNU as syntax) go through m68k-apple-macos-gcc; build-mac.sh passes -m68000
# in CMAKE_ASM_FLAGS so 68020+ instructions are rejected.
project(MoofTCPMac C ASM)

add_subdirectory(hello)
add_subdirectory(hello_init)
add_subdirectory(heapinfo)
add_subdirectory(apiprobe)
add_subdirectory(glue)
add_subdirectory(asmtest)
add_subdirectory(enetspike)
