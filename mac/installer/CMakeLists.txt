# The installer (Plan 3 Tasks 9 and 10; one installer for both variants since the "Lite checkbox"
# follow-up, docs/dev/DHCP-DESIGN.md section 10).
#
# mtinst68k: the portable planner and executor (inst_plan.c, inst_exec.c), built so the Retro68 gate
# compiles them with -m68000 -Werror (host tests: tests/test_inst_plan.c, tests/test_inst_exec.c).
add_library(mtinst68k STATIC inst_plan.c inst_exec.c)
target_include_directories(mtinst68k PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(mtinst68k PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=c99 -Wall -Wextra -Werror -O2>)

# The application is built in the full tree only. The Lite tree (MT_LITE=1, build-mac-lite/) is where
# the Lite MoofTCP file comes from; it has no installer of its own any more.
if(MT_NO_DHCP)
  return()
endif()

# Both MoofTCP files go inside the application: mkembed.py splits this tree's moof/MoofTCP.bin (full)
# and the Lite tree's (MT_LITE_MOOF_BIN, default build-mac-lite/moof/MoofTCP.bin beside this tree) into
# 'MTCP' / 'MTCD' / 'MTCI' 128 (full) and 129 (Lite), and checks each file's variant from its 'vers' 1.
# Rez makes embed.rsrc.bin, which add_application merges. Build order: both MoofTCP files first;
# scripts/build-mac.sh builds the Lite one (MT_LITE=1 ... MoofTCP) before any installer target.
set(MOOF_BIN ${CMAKE_BINARY_DIR}/moof/MoofTCP.bin)
set(MT_LITE_MOOF_BIN "${CMAKE_BINARY_DIR}/../build-mac-lite/moof/MoofTCP.bin" CACHE FILEPATH
    "The MoofTCP Lite file the installer embeds as id 129 (built with MT_NO_DHCP=ON in its own tree)")
set(EMBED ${CMAKE_CURRENT_BINARY_DIR}/embed)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/embed.rsrc.bin
    COMMAND python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/mkembed.py ${EMBED} ${MOOF_BIN} ${MT_LITE_MOOF_BIN}
    COMMAND ${REZ} ${EMBED}/embed.r -o ${CMAKE_CURRENT_BINARY_DIR}/embed.rsrc.bin
    DEPENDS ${MOOF_BIN} ${MT_LITE_MOOF_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/mkembed.py
            ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/resmap.py)

# installer.r through Rez here (not add_application's own rule), as before.
set(INST_RSRC ${CMAKE_CURRENT_BINARY_DIR}/installer.r.rsrc.bin)
add_custom_command(
    OUTPUT ${INST_RSRC}
    COMMAND ${REZ} -I${REZ_INCLUDE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/installer.r -o ${INST_RSRC}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/installer.r)

# "MoofTCP Installer" (type APPL, creator mtin, hasBundle). `scripts/build-mac.sh MoofInstaller` builds
# build-mac/installer/MoofTCP Installer.bin.
add_application(MoofInstallerApp installer_main.c inst_exec_mac.c ${INST_RSRC}
                ${CMAKE_CURRENT_BINARY_DIR}/embed.rsrc.bin CREATOR "mtin")
target_link_libraries(MoofInstallerApp mtinst68k)
target_compile_options(MoofInstallerApp PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=c99 -O2 -Wall -Wextra -Werror>)
add_dependencies(MoofInstallerApp_APPL MoofTCP_APPL)
add_custom_command(
    OUTPUT "MoofTCP Installer.bin"
    COMMAND ${CMAKE_COMMAND} -E copy MoofInstallerApp.bin "MoofTCP Installer.bin"
    COMMAND python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/mbflags.py "MoofTCP Installer.bin" 0x2000 "MoofTCP Installer"
    DEPENDS MoofInstallerApp.bin)
add_custom_target(MoofInstaller_APPL ALL DEPENDS "MoofTCP Installer.bin")

# Debug variant (never shipped): MT_INST_DEBUG=1 reads a stop-after-N-changes count from the file
# ":MoofTCP Installer Debug" at the root of the startup disk (the interruption test, "[I|R]N").
# build-mac/installer/Installer Dbg.bin.
add_application(MoofInstallerDbgApp installer_main.c inst_exec_mac.c
                ${INST_RSRC}
                ${CMAKE_CURRENT_BINARY_DIR}/embed.rsrc.bin CREATOR "mtin")
target_link_libraries(MoofInstallerDbgApp mtinst68k)
target_compile_definitions(MoofInstallerDbgApp PRIVATE MT_INST_DEBUG=1)
target_compile_options(MoofInstallerDbgApp PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=c99 -O2 -Wall -Wextra -Werror>)
add_dependencies(MoofInstallerDbgApp_APPL MoofInstallerApp_APPL)
add_custom_command(
    OUTPUT "Installer Dbg.bin"
    COMMAND ${CMAKE_COMMAND} -E copy MoofInstallerDbgApp.bin "Installer Dbg.bin"
    COMMAND python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/mbflags.py "Installer Dbg.bin" 0x2000 "Installer Dbg"
    DEPENDS MoofInstallerDbgApp.bin)
add_custom_target(MoofInstallerDbg_APPL ALL DEPENDS "Installer Dbg.bin")
