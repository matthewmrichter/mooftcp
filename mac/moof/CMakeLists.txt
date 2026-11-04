# MoofTCP: the control panel file "MoofTCP" (Task 10 driver, Plan 3 Task 8 control panel). Same
# recipe as mac/hello_init (flat Retro68 code per resource, wrapped by Rez):
#   MoofInit.flt  resource INIT 0 (mac/init/mt_init.c): loads the driver and calls its boot stub
#   MoofDrvr.flt  the code of resource DRVR 22 ".ipp" (moof.r puts the driver header and the
#                 entry stubs in front of it; mt_drv_stubs.s is the dispatcher)
#   MoofCdev.flt  resource cdev -4064, the control panel (mac/cdev)
#   MoofDnr.flt   resource dnrp 0, the resolver (mac/dnr)
# `scripts/build-mac.sh MoofTCP` builds build-mac/moof/MoofTCP.bin (MacBinary, cdev/ztcp, Finder flags
# 0x2000 hasBundle) and, for tests only, build-mac/moof/MacTCP DNR.bin (not shipped: Plan 3 ruling P1).
add_executable(MoofInit ${CMAKE_CURRENT_SOURCE_DIR}/../init/mt_init.c)
set_target_properties(MoofInit PROPERTIES OUTPUT_NAME MoofInit.flt)
target_include_directories(MoofInit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_options(MoofInit PRIVATE -Wl,--mac-flat)
target_compile_options(MoofInit PRIVATE -O2 -Wall -Wextra -Werror)

add_executable(MoofDrvr ${MT_DRV_STUBS})
set_target_properties(MoofDrvr PROPERTIES OUTPUT_NAME MoofDrvr.flt LINKER_LANGUAGE C)
target_link_libraries(MoofDrvr mtdrv68k mtglue68k mtcore68k)
target_link_options(MoofDrvr PRIVATE -Wl,--mac-flat)

add_custom_command(
    OUTPUT MoofTCP.bin MoofTCP.dsk
    COMMAND ${REZ} -I${REZ_INCLUDE_PATH} -I${CMAKE_CURRENT_SOURCE_DIR}
            -I${CMAKE_CURRENT_SOURCE_DIR}/../cdev ${MT_REZ_DEFS}
            ${CMAKE_CURRENT_SOURCE_DIR}/moof.r
            -o MoofTCP.bin --cc MoofTCP.dsk
            -t cdev -c ztcp
    COMMAND python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/mbflags.py MoofTCP.bin 0x2000
    DEPENDS MoofInit MoofDrvr MoofDnr MoofCdev moof.r dnrp.r
            ${CMAKE_CURRENT_SOURCE_DIR}/../cdev/moofcdev.r ${CMAKE_CURRENT_SOURCE_DIR}/../cdev/icons.r)
# "MacTCP DNR" (Task 11): the file apps find the resolver in, type cdev creator mtcp (the real file's).
# Kept for tests (ApiProbe group D, the DNR search with each file alone); the release ships only
# "MoofTCP", whose dnrp 0 apps find first (Task 8 resolver-alone run, docs/dev/APPS.md).
add_custom_command(
    OUTPUT "MacTCP DNR.bin" "MacTCP DNR.dsk"
    COMMAND ${REZ} -I${REZ_INCLUDE_PATH} -I${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/dnrfile.r
            -o "MacTCP DNR.bin" --cc "MacTCP DNR.dsk"
            -t cdev -c mtcp
    DEPENDS MoofDnr dnrfile.r dnrp.r)
# Named *_APPL so scripts/build-mac.sh builds the MacBinary for `MoofTCP`.
add_custom_target(MoofTCP_APPL ALL DEPENDS MoofTCP.bin "MacTCP DNR.bin")
