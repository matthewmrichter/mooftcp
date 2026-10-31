# The installer planner (Plan 3 Task 9): portable, so the Retro68 gate compiles it with
# -m68000 -Werror (host tests: tests/test_inst_plan.c). Task 10 adds the Toolbox executor.
add_library(mtinst68k STATIC inst_plan.c)
target_include_directories(mtinst68k PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(mtinst68k PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=c99 -Wall -Wextra -Werror -O2>)
