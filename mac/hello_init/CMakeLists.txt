# HelloInit: a System 6 INIT that draws its icon in the startup parade and
# installs nothing. It proves Retro68 code-resource builds; Plan 2's INIT and
# driver use the same recipe (see docs/dev/TOOLCHAIN.md).
#
# Not add_application(): that wraps the code as CODE 0/1 for an APPL. Instead
# link a plain executable with Elf2Mac's --mac-flat (one flat block of code plus
# Retro68's relocation info, entry _start at offset 0), then Rez $$read()s it
# into 'INIT' 128 and sets type/creator. Same layout as Retro68's
# Samples/SystemExtension.
add_executable(HelloInit init.c)
set_target_properties(HelloInit PROPERTIES OUTPUT_NAME HelloInit.flt)
target_link_options(HelloInit PRIVATE -Wl,--mac-flat)

add_custom_command(
    OUTPUT HelloInit.bin HelloInit.dsk
    COMMAND ${REZ} -I${REZ_INCLUDE_PATH}
            ${CMAKE_CURRENT_SOURCE_DIR}/hello_init.r
            -o HelloInit.bin --cc HelloInit.dsk
            -t INIT -c MTHI
    DEPENDS HelloInit hello_init.r)
# Named *_APPL so scripts/build-mac.sh builds the MacBinary for `HelloInit`.
add_custom_target(HelloInit_APPL ALL DEPENDS HelloInit.bin)
