# EnetSpike: Task 9 test app for the Ethernet (.ENET0 / DaynaPORT) driver glue. Attaches
# protocol handlers for 0x0806 and 0x0800, sends frames with EWrite, logs what the handler sees
# and writes EnetSpike.out beside the app. See docs/dev/MACTCP-INTERFACE.md "Ethernet driver calls".
add_application(EnetSpike enetspike.c spike_ph.s enetspike.r CREATOR "MTES")
target_compile_options(EnetSpike PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 -Wall -Wextra>)
