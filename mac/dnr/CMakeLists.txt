# MoofTCP's resolver: the 'dnrp' code resource (Task 11). Same recipe as the INIT and the driver:
# one flat Retro68 block, Rez wraps it (mac/moof/dnrp.r -> `dnrp` 0, in the "MacTCP" cdev file and in
# the file "MacTCP DNR"). mt_dnr_start.s defines `_start`, the entry the application calls.
add_executable(MoofDnr ${CMAKE_CURRENT_SOURCE_DIR}/mt_dnr.c ${CMAKE_CURRENT_SOURCE_DIR}/mt_dnr_start.s)
set_target_properties(MoofDnr PROPERTIES OUTPUT_NAME MoofDnr.flt LINKER_LANGUAGE C)
target_include_directories(MoofDnr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_options(MoofDnr PRIVATE -Wl,--mac-flat)
target_compile_options(MoofDnr PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=c99 -O2 -Wall -Wextra -Werror>)
