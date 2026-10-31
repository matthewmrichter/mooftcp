# MoofTCP: the interim cdev file "MacTCP" (Task 10; Plan 3 adds the control panel UI). Same
# recipe as mac/hello_init (flat Retro68 code per resource, wrapped by Rez):
#   MoofInit.flt  resource INIT 0 (mac/init/mt_init.c): loads the driver and calls its boot stub
#   MoofDrvr.flt  the code of resource DRVR 22 ".ipp" (moof.r puts the driver header and the
#                 entry stubs in front of it; mt_drv_stubs.s is the dispatcher)
# `scripts/build-mac.sh MoofTCP` builds build-mac/moof/MacTCP.bin (MacBinary, cdev/ztcp).
add_executable(MoofInit ${CMAKE_CURRENT_SOURCE_DIR}/../init/mt_init.c)
set_target_properties(MoofInit PROPERTIES OUTPUT_NAME MoofInit.flt)
target_link_options(MoofInit PRIVATE -Wl,--mac-flat)
target_compile_options(MoofInit PRIVATE -O2 -Wall -Wextra -Werror)

add_executable(MoofDrvr ${MT_DRV_STUBS})
set_target_properties(MoofDrvr PROPERTIES OUTPUT_NAME MoofDrvr.flt LINKER_LANGUAGE C)
target_link_libraries(MoofDrvr mtdrv68k mtglue68k mtcore68k)
target_link_options(MoofDrvr PRIVATE -Wl,--mac-flat)

add_custom_command(
    OUTPUT MacTCP.bin MacTCP.dsk
    COMMAND ${REZ} -I${REZ_INCLUDE_PATH}
            ${CMAKE_CURRENT_SOURCE_DIR}/moof.r
            -o MacTCP.bin --cc MacTCP.dsk
            -t cdev -c ztcp
    DEPENDS MoofInit MoofDrvr moof.r)
# Named *_APPL so scripts/build-mac.sh builds the MacBinary for `MoofTCP`.
add_custom_target(MoofTCP_APPL ALL DEPENDS MacTCP.bin)
