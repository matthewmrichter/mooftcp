# MoofDiag: reads the installed .IPP driver's state (unit table entry, OpenDriver result, and through
# the driver's private Status call the resident block: boot results, gate/Ethernet/core counters) and
# writes MoofDiag.out beside the app. Run with scripts/moofdiag-run.sh (Task 10).
add_application(MoofDiag moofdiag.c diag_streams.c ../common/runid.c ../common/runid_parse.c moofdiag.r CREATOR "MTMD")
target_link_libraries(MoofDiag mtglue68k mtcore68k)
target_compile_options(MoofDiag PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 -Wall -Wextra>)
target_include_directories(MoofDiag PRIVATE ../common)
