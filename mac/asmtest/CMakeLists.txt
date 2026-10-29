# AsmTest: checks mac/glue/mt_cksum68k.s against the C versions of core/mt_util.c (compiled
# into the app under other names by cref.c) over every src/dst alignment and the lengths that
# matter, times both, and writes AsmTest.out beside the app. -O2: the C side is timed as the
# optimised core would run.
add_application(AsmTest asmtest.c cref.c ${MT_CKSUM68K} asmtest.r CREATOR "MTAS")
target_include_directories(AsmTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../core)
target_compile_options(AsmTest PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 -Wall -Wextra>)
