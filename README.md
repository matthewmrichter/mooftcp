# MoofTCP

**A modern, memory-lean replacement for MacTCP on 68k Macs running System 6.**

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Platform: System 6 / 68000](https://img.shields.io/badge/platform-System%206%20%2F%2068000-lightgrey.svg)
![Version 1.0](https://img.shields.io/badge/version-1.0-brightgreen.svg)

MoofTCP is a drop-in replacement for Apple's MacTCP. Programs written for MacTCP run on it unchanged, but
underneath it is a fresh TCP/IP stack written from scratch in portable C, with its own control panel, installer
and DHCP client. It was built for a Mac Classic with 4 MB of RAM, so every byte counts.

![The MoofTCP control panel](docs/images/panel.png)

## Why?

MacTCP is wonderful, but it is old, and on a 4 MB Mac Classic with a SCSI Wi-Fi/Ethernet adapter like
BlueSCSI it can be slow to answer, hungry for memory, and awkward to set up. MoofTCP aims, in this order, for:

1. **Reliability.** No hangs, no leaks, no surprises when apps quit.
2. **Low latency** for interactive use, like typing in a telnet session.
3. **Throughput**, for handling quasi-modern use cases.

...all while using **no more resident memory than MacTCP does**.

## Features

- **Drop-in compatible.** Implements the MacTCP `.IPP` driver interface (TCP, UDP, ICMP echo) and the `dnrp`
  name resolver. Your existing MacTCP programs just work.
- **DHCP or a fixed address.** Pick either in the control panel. Don't need to manage static IP anymore.
- **MoofTCP Lite.** A build without DHCP that saves about 5 KB of system memory, for tight machines. One
  installer holds both; a checkbox picks which one.
- **A friendly installer.** It moves MacTCP's files aside, installs MoofTCP, and its Remove button puts
  everything back exactly as it was. An interrupted install repairs itself.
- **MultiFinder-aware.** Quitting an app that had connections open cleans up after it.
- **Built-in diagnostics.** An optional in-memory log and a small `MoofDiag` tool for bug reports.
- **A portable core.** The TCP/IP stack in `core/` has no `malloc` and no global state, and is tested on the
  desktop with sanitizers and fuzzers.

## Screenshots

| Installer                                   | MoofTCP Lite panel                            |
| ------------------------------------------- | --------------------------------------------- |
| ![The installer](docs/images/installer.png) | ![The Lite panel](docs/images/panel-lite.png) |

## Quick start

1. Download `MoofTCP-1.0.dsk`, `MoofTCP-1.0.hqx` or `MoofTCP-1.0.sit` from the
   [Releases](../../releases) page and get it onto your Mac (a BlueSCSI shared folder works well).
2. Open **MoofTCP Installer**, leave **Lite version (no DHCP)** unticked, and click **Install**.
3. Restart the Mac.
4. Open **Control Panels, MoofTCP**, enter your IP address, mask, router and DNS server (or tick
   **Get the address by DHCP**), and save.
5. Restart again. You're online.

To go back to MacTCP, run the installer and click **Remove**. The `Read Me` file in the release has the
details, including how to switch between MoofTCP and MoofTCP Lite.

StuffIt Lite 3.6 is tested as working for expanding the `.hqx` packages.

### What you need

- A 68k Mac with **System 6.0.8** (the version it was tested on). Please report back if it works on other versions!
- An Ethernet adapter that provides the `.ENET0` driver, such as a **BlueSCSI v2 WiFi with DaynaPORT**. MoofTCP
  does not include an Ethernet driver.

## Compatibility

Tested on a real **Mac Classic** (System 6.0.8, 4 MB, BlueSCSI v2 WiFi with DaynaPORT) and in the
[Snow](https://github.com/twvd/snow) emulator.

| Program                 | Result                       |
| ----------------------- | ---------------------------- |
| Flynn (telnet terminal) | Works, real Mac and emulator |
| Wikipedia Reader        | Works, real Mac and emulator |
| MacTCP Watcher          | Works in the emulator        |
| NCSA Telnet             | Works in the emulator        |
| Fetch (FTP)             | Works in the emulator        |
| TurboGopher             | Works in the emulator        |
| Eudora                  | Starts; mail was not tried   |

Have a program that doesn't work? Please open an issue (see below).

## Memory

|                                                                                 | MoofTCP       | MoofTCP Lite |
| ------------------------------------------------------------------------------- | ------------- | ------------ |
| Driver code                                                                     | 95,623 bytes  | 90,722 bytes |
| Free system heap after first open (real Mac Classic, single Finder, full build) | about 30.7 KB | n/a          |

MultiFinder grows the system heap on demand, so free memory varies more there.

## Something went wrong?

MoofTCP keeps a small diagnostic log in memory. If a program misbehaves but the Mac keeps running, run
**MoofDiag** right away (it is in the release's diagnostic tools download), and open an issue with the
`MoofDiag.out` file it writes next to itself, plus a note about what you were doing. Feel free to ask your LLM
to analyze the codebase and the diagnostic output as well and to submit fix PRs.

## Known limitations

- No IPv6.
- Tested only with the DaynaPORT Ethernet driver on System 6.0.8.
- DHCP is basic: no lease RELEASE, no INIT-REBOOT and no address-conflict probe. While it runs, programs cannot
  use UDP port 68. With no DHCP server, the first open waits about 10 seconds and then fails.
  See [`docs/dev/DHCP-DESIGN.md`](docs/dev/DHCP-DESIGN.md).

## Building from source

You only need Docker. The pinned [Retro68](https://github.com/autc04/Retro68) cross-compiler image is pulled
automatically.

```sh
scripts/build-mac.sh MoofTCP              # the MoofTCP file -> build-mac/moof/
MT_LITE=1 scripts/build-mac.sh MoofTCP    # MoofTCP Lite -> build-mac-lite/
scripts/release.sh --force 1.0            # one installer with both -> dist/MoofTCP-1.0.{dsk,hqx,sit}
```

`release.sh` also needs hfsutils, macutils, unar, gcc and Python 3.

### Running the tests

The portable core and the glue logic are tested on your desktop with gcc and CMake (AddressSanitizer and
UBSan are on by default):

```sh
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
cmake -S . -B build-lite -DMT_NO_DHCP=ON   # the Lite configuration
```

The emulator tests (Snow) need Apple system software and the BlueSCSI DaynaPORT image. Those are not in this
repository and you must supply them yourself; see [`docs/dev/EMULATOR.md`](docs/dev/EMULATOR.md) and
[`docs/dev/GOLDEN-IMAGE.md`](docs/dev/GOLDEN-IMAGE.md).

## Project layout

```
core/      portable TCP/IP stack (no malloc, no globals), host-tested
mac/       Mac glue: .IPP driver, INIT, resolver, control panel, installer, diagnostic apps
tests/     host tests (ctest), including fuzzers
scripts/   build, release, emulator and hardware-session scripts
docs/dev/  design notes, Mac interface notes, hardware test specs, the DHCP design
tools/     pinned tool versions; tools/sit is a small StuffIt-format writer
```

## Contributing

Bug reports, compatibility reports ("program X does/doesn't work") and pull requests are welcome. For code
changes, please run the host tests first (see above). The developer docs in `docs/dev/` describe the design,
and `CLAUDE.md` is a detailed map of the code and its rules.

## Credits

- [Retro68](https://github.com/autc04/Retro68) for the 68k cross-compiler.
- [Snow](https://github.com/twvd/snow) for the Mac emulator the tests run in.
- [BlueSCSI](https://bluescsi.com) and DaynaPORT for the network hardware this was built around.
- [Claude Code](https://claude.ai) was used to develop this project, with human planning and testing.

## TODO

- More comprehensive bake-in testing on real hardware as we use it
- Remake Daynaport similarly (stay tuned for _MoofPort_)
- Review the BlueSCSI codebase for opportunities to integrate more tightly for better performance
- There is potential for further algorithm optimization for added performance on smaller or older systems.

## License

[MIT](LICENSE). "MacTCP" and "StuffIt" are trademarks of their owners and are used here only to describe
compatibility. MoofTCP contains no Apple or third-party code; building it uses Apple's interfaces as shipped in
the Retro68 toolchain.
