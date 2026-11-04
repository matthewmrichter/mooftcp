# KeyLat: keypress-to-wire latency probe for the real Mac (a minimal interactive TCP client). kl_os.c holds
# all Toolbox includes (they clash with mt_mactcp.h); keylat.c uses only mt_mactcp.h. Host side: scripts/keylatsrv.py.
add_application(KeyLat keylat.c kl_os.c keylat.r CREATOR "MTKL")
target_include_directories(KeyLat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_options(KeyLat PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 -Wall -Wextra>)
