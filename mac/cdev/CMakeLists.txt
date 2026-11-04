# The MoofTCP control panel (Plan 3 Tasks 7 and 8).
#
# mtcdev68k: the portable field logic (cfg_edit.c), built so the Retro68 gate compiles it with
# -m68000 -Werror (the host tests are tests/test_cfg_edit.c).
add_library(mtcdev68k STATIC cfg_edit.c cdev_logic.c)
target_include_directories(mtcdev68k PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../glue ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../core)
target_compile_options(mtcdev68k PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=c99 -Wall -Wextra -Werror -O2>)

# MoofCdev.flt: the 'cdev' -4064 code resource (cdev_start.s is the Pascal entry, cdev_main.c the
# panel; cfg_edit.c compiled again into it). Same flat-code recipe as the INIT and the DNR, but
# position independent: -mpcrel makes every code and data reference PC-relative and the code never
# calls Retro68Relocate, so it runs wherever the Control Panel loads it, even if the handle moves
# between messages. -fno-tree-switch-conversion: a switch must not become a table of pointers (data
# relocations). The POST_BUILD step fails the build if any object of ours carries an absolute
# relocation (R_68K_32), i.e. something that would need Retro68Relocate.
add_executable(MoofCdev cdev_start.s cdev_main.c cdev_logic.c cfg_edit.c)
set_target_properties(MoofCdev PROPERTIES OUTPUT_NAME MoofCdev.flt LINKER_LANGUAGE C)
target_include_directories(MoofCdev PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../glue ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../core)
target_compile_options(MoofCdev PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=c99 -O2 -Wall -Wextra -Werror -mpcrel -fno-tree-switch-conversion>)
target_link_options(MoofCdev PRIVATE -Wl,--mac-flat)
add_custom_command(TARGET MoofCdev POST_BUILD
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/check_pcrel.sh ${CMAKE_OBJDUMP}
          $<TARGET_FILE:MoofCdev>.gdb
          ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/MoofCdev.dir
  VERBATIM)
