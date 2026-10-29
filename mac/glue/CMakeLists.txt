# Mac glue. MT_CKSUM68K is the 68000 checksum/copy (mt_cksum_add, mt_cksum_copy); any target
# that links it must compile core/mt_util.c with MT_ASM_CKSUM (mtcore68k does).
set(MT_CKSUM68K ${CMAKE_CURRENT_SOURCE_DIR}/mt_cksum68k.s PARENT_SCOPE)
set(MT_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../core)

# The portable core built for the 68000 with the asm override (for the driver/INIT glue).
file(GLOB MT_CORE68K_SRC CONFIGURE_DEPENDS ${MT_CORE_DIR}/*.c)
add_library(mtcore68k STATIC ${MT_CORE68K_SRC} mt_cksum68k.s)
target_include_directories(mtcore68k PUBLIC ${MT_CORE_DIR})
target_compile_definitions(mtcore68k PUBLIC MT_ASM_CKSUM)
target_compile_options(mtcore68k PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=c99 -Wall -Wextra -Werror -O2>)
