# MoofTCPTune (perf builds): sets the MoofTCP timing levers through the driver's private tune Control call
# (mac/glue/mt_tune_abi.h) from a preset named by the run id or MoofTCPTune.cfg, writes
# MoofTCPTune-<id>.out. Run unattended with a "!!<id>" run file (mac/common/runid.h). A release driver refuses
# the call and the run ends RESULT FAIL.
add_application(MoofTCPTune mooftcptune.c tune_parse.c ../common/runid.c ../common/runid_parse.c mooftcptune.r CREATOR "MTTN")
target_link_libraries(MoofTCPTune mtglue68k mtcore68k)
target_compile_options(MoofTCPTune PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 -Wall -Wextra>)
target_include_directories(MoofTCPTune PRIVATE ../common)
