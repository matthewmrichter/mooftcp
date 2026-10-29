# ApiProbe: runs a fixed list of MacTCP API cases against whichever stack is loaded and
# writes one line per case to ApiProbe.out. probe_os.c holds all Toolbox includes (they
# clash with mt_mactcp.h); apiprobe.c uses only mt_mactcp.h.
add_application(ApiProbe apiprobe.c probe_os.c apiprobe.r CREATOR "MTAP")
target_include_directories(ApiProbe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
