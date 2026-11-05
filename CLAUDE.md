# CLAUDE.md

Guidance for Claude Code (and other coding agents) working in this repository.

MoofTCP is a drop-in replacement for MacTCP on 68k Macs running System 6 (developed for a Mac Classic: System 6.0.8,
4 MB, 68000). Priorities, in order: reliability, interactive latency, throughput, all within resident memory no larger
than MacTCP's. Release 1.0 ships one installer holding two builds: the full build (with a DHCP client) and
**MoofTCP Lite** (compile option `MT_NO_DHCP`, about 5 KB less system heap).

Design notes are not part of the repository; the code comments and the header files (`core/mt.h`, `core/mt_int.h`,
`mac/glue/mt_pb.h`) are the reference. A comment that cites a `docs/dev/` file refers to maintainer notes kept outside the repo.

## Repository map

    core/       portable C99 TCP/IP stack (no malloc, no globals), host-tested; public API core/mt.h, internals mt_int.h
    mac/glue/   .IPP PB dispatcher over the core (mt_pb*.c), driver entry (mt_drv*), prefs (mt_cfg), Ethernet (mt_enet*)
    mac/init/   INIT 0           mac/dnr/  dnrp resolver        mac/moof/  moof.r: the "MoofTCP" file (DRVR 22, sysz, ...)
    mac/cdev/   control panel (cfg_edit, cdev_logic pure and host-tested; cdev_main Toolbox glue)
    mac/installer/  installer (inst_plan portable planner, inst_exec executor, UI); embeds both builds via scripts/mkembed.py
    mac/*       test and diagnostic apps: MoofDiag, HeapInfo*, ApiProbe, KeyLat, MfEcho, AsmTest, EnetSpike, Hello*
    tests/      host tests (ctest), fuzzers          scripts/  build, release, emulator and hardware-session tooling
    tools/      pinned tool versions; tools/sit is a StuffIt-format writer (MIT)

## Bootstrap (clean machine)

**Quick start:** the host tests and the Mac builds need only Docker (plus gcc and CMake for the host tests); nothing below
is required for them. Steps 2-5 are only for the Snow emulator tests, which need your own ROMs and Apple system software.

1. Needs `sudo` (the maintainer does this; you must also be in the `docker` group):

       sudo apt install docker.io xvfb x11-utils python3 python3-cryptography curl unzip \
           hfsutils macutils unar xdotool scrot

   (`tcpdump` is optional.)
2. `scripts/fetch-snow.sh` (pinned Snow release, SHA-256 verified).
3. `scripts/setup-home.sh` builds the `MT_HOME` tree. It needs your own ROMs and media, and the BlueSCSI DaynaPORT 7.5.3
   bootstrap zip (`fetch-daynaport753.sh`: put the zip where `DP753_LOCAL` points, or give a public link in `DP753_URL`;
   no link is pinned because it contains Apple software).
4. Golden test image: `scripts/golden-stage.sh`, ending with `scripts/golden-promote.sh` (needs your own System 6.0.8 and MacTCP media).
5. `scripts/fresh-disk.sh`, then `scripts/build-mac.sh Hello`.


## Commands

    cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure   # host tests (ASan+UBSan)
    cmake -S . -B build-lite -DMT_NO_DHCP=ON                                                   # Lite configuration
    scripts/build-mac.sh <Target>      # Retro68 in Docker -> build-mac/<dir>/<Target>.bin; MT_LITE=1 builds Lite in build-mac-lite/
                                       # Targets: MoofTCP, MoofInstaller, MoofDiag, HeapInfo, HeapInfoOpen, HeapInfoDNR, ApiProbe, KeyLat, ...
    scripts/gate-check.sh              # every Mac target compiled -m68000 -O2 -Wall -Wextra -Werror (all configurations)
    scripts/check-all.sh [--snow]      # host ctest (full and Lite) + gate-check; --snow adds the emulator checks below
    scripts/release.sh [--force] <ver> # -> dist/MoofTCP-<ver>.{dsk,hqx,sit} (one installer with both builds); then
    scripts/release-verify.sh <ver>    # reads every format back and compares forks, flags and both embedded builds
    scripts/stage-hw.sh --release <ver> <session>   # dist/hw/<session>/: release files plus the diagnostic tools for a hardware session

Snow (emulator) helpers: `snow.sh start|stop|status|shot`, `fresh-disk.sh`, `deploy.sh`, `install-moof.sh` (dev install on
work.hda), `installer-test.sh scenario <...>` (installer through the GUI), `mem-check.sh` (resident-memory cap rules),
`moofdiag-run.sh`, `probe-run.sh` (ApiProbe, `--moof` for MoofTCP), `mf-run.sh` (MultiFinder), `soak-run.sh`, `lat-run.sh`,
`keylat-run.sh`, `dhcp-tap-run.sh`. Each script's header explains its options. `MT_LITE=1` selects the Lite tree in the ones
that install or build.

## `MT_HOME` (default `~/.local/share/mooftcp`, never in git)

    snow/ pinned snowemu     rom/ ROMs     disks/ golden.hda (read-only), work.hda
    media/ system/MacTCP images, DaynaPORT, apps/<app>/ (fetch-apps.sh)     pylib/ ftp/ helpers

## Rules

- Snow runs on a private Xvfb `:142` (the scripts accept `:140`-`:149`), never on the maintainer's desktop. Use `scripts/snow.sh`.
  One Snow at a time; stop it by recorded PID, never `pkill -f snowemu`. Never edit a disk image while Snow runs.
- Never mount `golden.hda`; only `work.hda`, with Snow stopped. Golden changes go through `scripts/golden-promote.sh`.
- MoofTCP must match MacTCP only at the API boundary (`.IPP` calls, PB layouts, error codes, ASR timing, the DNR resource).
  Internals are MoofTCP's own design; do not clone MacTCP's engine. `EMULATOR.md` "Observed reference behavior" records
  what apps tolerate. Resident memory must not exceed MacTCP's (baseline in `EMULATOR.md`; `scripts/mem-check.sh`).
- Nothing Apple-copyrighted or large in git: no ROMs, System software, MacTCP, disk images or built binaries.
- GUI clicks in Snow: `xdotool mousedown 1; sleep 0.05; xdotool mouseup 1` with incremental motion; focus before typing.
- Anything needing `sudo` is run by the maintainer, not by an agent.

## Working with agents

When an AI agent works in the maintainer's checkout: do not push, tag, release or change repository settings without the
maintainer's explicit approval, and ask before any action that is hard to reverse. Contributors working in their own fork
are not bound by this.

## Gate rule for Mac glue

Glue reachable from `mt_pb_*` runs inside the core's owner gate (`mt_gate.h`): enter the core only via `MT_GATE_ENTER/LEAVE`
or the handler/timer paths, never re-enter `mt_input`/`mt_tick`, make no Memory Manager calls in callbacks or at interrupt
time, route every OS service through `mac/include/mt_os.h`, keep residents locked in the system heap, add a host test
against `tests/pbfake.[ch]`, and run `test_fuzz_pb`. Driver growth needs `scripts/mem-check.sh` and `scripts/probe-run.sh --moof`.
Interrupt-time stack budget is 4352 B (`MACTCP-INTERFACE.md` "Stack budget"); any change that deepens the call path must
re-measure (`MT_EXTRA_CFLAGS="-DMT_STACK_MARK=1 -DMT_STACK_PAINT=4096" scripts/build-mac.sh <Target>`, MoofDiag `stack:` line).
Control panel code is position independent (`-mpcrel`, no globals, no pointer tables; `check_pcrel.sh` enforces it), and it
must never touch dialog items at `closeDev`: Control Panel removes them before sending it.

## Portable core (`core/`)

Constraints:
- No `malloc`, no mutable globals; all state in caller-provided structs (one `mt_stack` in one caller-supplied arena).
- Packet bytes only through `mt_rd16/32`, `mt_wr16/32` on `uint8_t *`; fixed-width `stdint.h` types.
- The ms clock and TCP sequence numbers are compared only with `mt_time_before` / `mt_seq_*` (wrap-safe).
- No blocking in the core; it is driven by calls and `mt_tick(now)` when `mt_next_deadline(now)` reaches 0 (re-query after
  EVERY API call). Every call finishes in bounded time.
- Build flags `-std=c99 -Wall -Wextra -Werror`; the core must also compile with Retro68's `m68k-apple-macos-gcc` (no GNU
  extensions). Tests run with ASan+UBSan; any report is a failure.
- Memory is budgeted: a new arena section must keep `mt_stack_size()` <= 18432 (`test_stack_size_budget`) and resident
  memory below MacTCP's.

Contract highlights (full text in `core/mt.h`):
- Frames in: `mt_input(frame, len, now)`. Frames out: `mt_link.tx`, called synchronously with a complete Ethernet frame;
  a nonzero return means "busy now" (TCP stalls and retries, see `tcp_tx_stalls`). IP frames may be under 60 bytes; the Mac
  glue pads. `link.tx` must not call back into the stack.
- `mt_input` and `mt_tick` are never re-entered; every other call is legal from callbacks. Callbacks fire synchronously in
  the caller's context (possibly interrupt time). Callback pointers are valid only during the callback.
- TCP sends are zero-copy (`mt_tcp_send_segs`): the segment array and bytes must stay unmodified until `done` fires. A
  callback may release its object; re-check `gen` after `fire()`.
- Loopback: datagrams to our own address or 127/8 go through a small FIFO delivered by the next `mt_tick`.
- Diagnostics: counters via `mt_get_stats` (each counter bumped exactly once per event it names;
  `test_stats_all_counters_live`), an optional 128-record log (`mt_log_enable`), drop sites use `MT_DROP_IP` / `MT_DROP_GOTO` /
  `MT_LOG_DROPPED`.
- DHCP client (`core/mt_dhcp.c`, absent with `MT_NO_DHCP`): RFC 2131 without INIT-REBOOT, ARP probe or RELEASE (rulings in
  `DHCP-DESIGN.md`).

Review focus for core changes: hot paths (per-packet input/output, TCP send/receive) get no new work; a TCB state change
goes through `mt_tcp_set_state`; widen before shifting; new modules follow the checklist at the top of `mt_int.h`.

## Mac layer notes

- Driver: the "MoofTCP" file holds INIT 0, DRVR 22 `.ipp`, `dnrp` 0, `sysz` and the control panel. `.ENET0` is opened by the
  first `OpenDriver(".IPP")`, not at INIT time (the DaynaPORT driver crashes before the first app runs). No address, or DHCP
  without a lease, makes the open fail with -23004. MultiFinder sends no goodbye when an application quits; the glue
  detects dead partitions and cleans up (`mt_pb.c` "per-application cleanup", `EMULATOR.md` "MultiFinder").
- Prefs: `MoofTCP Prefs`, MCFG 128 (format in `mac/glue/mt_cfg.h`); private Status csCode 0x4D54 returns the resident block for
  MoofDiag. Lite ignores the DHCP mode word and runs the prefs as static.
- Installer: the planner (`inst_plan`) and executor (`inst_exec`) are portable and host-tested including failure injection at
  every file system change; the Mac table is `inst_exec_mac`. Both builds are embedded ('MTCP' 128 full, 129 Lite) and the
  checkbox picks which Install writes; switching variants needs no Remove.
- Versions: `vers` resources use release stage (`scripts/relvers.py` checks the short string and variant).

## Tests

`ctest --test-dir build` runs about 40 tests: core modules (`test_util`, `test_arp`, `test_ip`, `test_icmp`, `test_udp`,
`test_dns*`, `test_tcp_*`, `test_dhcp`, `test_harden`, `test_pair`, `test_stack`, ...), the fuzzers (`test_fuzz`,
`test_fuzz_pb`; `FZ_SEED=n` / `FZ_SEEDS=n`), the PB glue against the fake OS (`test_pb_*`, `test_drv`, `test_pb_multi`,
`test_gate`, `test_enet_*`, `test_loopback`), prefs and panel logic (`test_cfg_*`, `test_cdev_logic`) and the installer
(`test_inst_plan`, `test_inst_exec`). Run one with `./build/test_util <name substring>`. The Lite configuration (`build-lite`)
leaves out the DHCP-only tests. `T_MAIN` takes at most 32 tests per file (`tests/t.h`).
