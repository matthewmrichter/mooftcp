# Control panel portable field logic (Plan 3 Task 7). Built here so the Retro68 gate compiles it
# with -m68000 -Werror; the Toolbox cdev itself arrives in Task 8.
add_library(mtcdev68k STATIC cfg_edit.c)
target_include_directories(mtcdev68k PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../glue ${CMAKE_CURRENT_SOURCE_DIR}/../../core)
target_compile_options(mtcdev68k PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=c99 -Wall -Wextra -Werror -O2>)
