# MfEcho: a TCP echo client for the MultiFinder per-application tests (Plan 3 Task 12). Copies named
# MfEchoA / MfEchoB run side by side; each writes "<name>.out" beside itself. scripts/mf-run.sh drives it.
add_application(MfEcho mfecho.c mf_os.c mfecho.r CREATOR "MTMF")
target_link_libraries(MfEcho mtglue68k mtcore68k)
target_compile_options(MfEcho PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 -Wall -Wextra>)
