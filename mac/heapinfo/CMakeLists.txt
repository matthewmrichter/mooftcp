add_application(HeapInfo heapinfo.c heapinfo.r CREATOR "MTHP")

# Same source, built with -DOPEN_IPP: opens the MacTCP driver before measuring.
add_application(HeapInfoOpen heapinfo.c heapinfo.r CREATOR "MTHQ")
target_compile_definitions(HeapInfoOpen PRIVATE OPEN_IPP)
