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

# The Mac Ethernet/clock/gate glue (Task 9). mt_enet.c and mt_timer.c include Toolbox headers;
# nothing here includes mt_mactcp.h next to them (the two clash), mt_os_mac.c only mt_os.h.
add_library(mtglue68k STATIC
  mt_gate.c mt_enet.c mt_enet_ph.s mt_timer.c mt_timer_tm.s mt_os_mac.c)
target_include_directories(mtglue68k PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_definitions(mtglue68k PUBLIC MT_OS_MAC)
target_compile_options(mtglue68k PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=c99 -Wall -Wextra -Werror -O2>)
