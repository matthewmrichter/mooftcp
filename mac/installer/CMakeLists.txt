# The installer (Plan 3 Tasks 9 and 10).
#
# mtinst68k: the portable planner and executor (inst_plan.c, inst_exec.c), built so the Retro68 gate
# compiles them with -m68000 -Werror (host tests: tests/test_inst_plan.c, tests/test_inst_exec.c).
add_library(mtinst68k STATIC inst_plan.c inst_exec.c)
target_include_directories(mtinst68k PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(mtinst68k PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=c99 -Wall -Wextra -Werror -O2>)

# The MoofTCP file goes inside the application: mkembed.py splits build-mac/moof/MoofTCP.bin into
# 'MTCP' 128 (resource fork), 'MTCD' 128 (data fork, only if any) and 'MTCI' 128 (type, creator,
# Finder flags); Rez makes embed.rsrc.bin, which add_application merges. Build order: MoofTCP first.
set(MOOF_BIN ${CMAKE_BINARY_DIR}/moof/MoofTCP.bin)
set(EMBED ${CMAKE_CURRENT_BINARY_DIR}/embed)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/embed.rsrc.bin
    COMMAND python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/mkembed.py ${MOOF_BIN} ${EMBED}
    COMMAND ${REZ} ${EMBED}/embed.r -o ${CMAKE_CURRENT_BINARY_DIR}/embed.rsrc.bin
    DEPENDS ${MOOF_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/mkembed.py)

# installer.r goes through Rez here (not add_application's own rule) so MT_REZ_DEFS reach it: MoofTCP
# Lite (-DMT_NO_DHCP) names itself "Lite" in the window, the texts and the 'vers' resources.
set(INST_RSRC ${CMAKE_CURRENT_BINARY_DIR}/installer.r.rsrc.bin)
add_custom_command(
    OUTPUT ${INST_RSRC}
    COMMAND ${REZ} -I${REZ_INCLUDE_PATH} ${MT_REZ_DEFS} ${CMAKE_CURRENT_SOURCE_DIR}/installer.r -o ${INST_RSRC}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/installer.r)

# "MoofTCP Installer" (type APPL, creator mtin, hasBundle). `scripts/build-mac.sh MoofInstaller` builds
# build-mac/installer/MoofTCP Installer.bin (MT_LITE=1: build-mac-lite/installer/, the Lite installer). The
# build file keeps that name in both trees; the name on the Mac (the MacBinary header) is "MoofTCP Lite
# Installer" in Lite, so both installers can sit in one folder.
if(MT_NO_DHCP)
  set(INST_APP_NAME "MoofTCP Lite Installer")
else()
  set(INST_APP_NAME "MoofTCP Installer")
endif()
add_application(MoofInstallerApp installer_main.c inst_exec_mac.c ${INST_RSRC}
                ${CMAKE_CURRENT_BINARY_DIR}/embed.rsrc.bin CREATOR "mtin")
target_link_libraries(MoofInstallerApp mtinst68k)
target_compile_options(MoofInstallerApp PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=c99 -O2 -Wall -Wextra -Werror>)
add_dependencies(MoofInstallerApp_APPL MoofTCP_APPL)
add_custom_command(
    OUTPUT "MoofTCP Installer.bin"
    COMMAND ${CMAKE_COMMAND} -E copy MoofInstallerApp.bin "MoofTCP Installer.bin"
    COMMAND python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/mbflags.py "MoofTCP Installer.bin" 0x2000 "${INST_APP_NAME}"
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
