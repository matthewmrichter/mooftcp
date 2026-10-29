# EnetSpike: Task 9 test app for the Ethernet (.ENET0 / DaynaPORT) glue. It runs the core stack
# on mac/glue's Ethernet, Time Manager and gate modules against Snow's NAT gateway and writes
# EnetSpike.out beside the app. See docs/dev/MACTCP-INTERFACE.md "Ethernet driver calls". (The
# first form of this app, the hand-driven spike, is in git history: commit "Ethernet driver spike".)
add_application(EnetSpike enetspike.c enetspike.r CREATOR "MTES")
target_link_libraries(EnetSpike mtglue68k mtcore68k)
target_compile_options(EnetSpike PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 -Wall -Wextra>)
