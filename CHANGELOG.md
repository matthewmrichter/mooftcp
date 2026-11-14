# Changelog

## 1.1rc1 (release candidate, 2026-11-13)

A faster MoofTCP. The programs, settings file and installer work as in 1.0.

### Faster

Measured on a real Mac Classic (System 6.0.8, 4 MB, BlueSCSI v2 Wi-Fi), timed on the PC. The table compares
MoofTCP 1.1 with MoofPort 1.1 against 1.0's behaviour (the MoofTCP 1.0 engine, with MoofPort at 1.0's settings).
The gains come from both together: MoofTCP 1.1's shorter call path and MoofPort's `steadyack` timing, which
MoofTCP 1.1 switches on. Medians:

| | 1.0 | 1.1 |
|---|---|---|
| Echo round trip, 1 byte | 53.6 ms | 41.4 ms |
| Terminal burst, 2,000 bytes | 153.6 ms | 124.6 ms |
| TCP receive, 1 MB | 45.3 KB/s | 55.6 KB/s |

Measured 2026-11-14 in one session of 10 pairs, with the AppleShare server's Wi-Fi power saving off. These three
were better in all 10 pairs (the session the night before gave the same direction). Not significant in this
session: TCP send with 4 sends queued (36.7 -> 41.0 KB/s), one-at-a-time sends (25.2 -> 23.9 KB/s) and AppleShare
copies (too few pairs with the volume mounted). The network was noisy that day: stock MacTCP's one-at-a-time send
ranged from 11 to 40 KB/s. The figures were measured over Wi-Fi and vary with the network.

With the DaynaPORT driver the gain is small, because MoofTCP leaves DaynaPORT's driver as it is: echo about
54.9 ms with MoofTCP 1.1 against 57.6 ms with 1.0 (two blocks on 2026-11-14; the 1.0 figure is from an earlier
session with different Wi-Fi settings, so this is not a clean comparison).

### Changed

- A shorter path through every MacTCP call and every packet: less work per call, per timer tick and per frame.
- When the Ethernet driver is MoofPort 1.1, MoofTCP switches it to MoofPort's `steadyack` timing when it opens the
  driver. With any other driver (DaynaPORT, for example) it changes nothing (MoofDiag shows `tune 0`).
- The timer recovers by itself if a Time Manager task is lost, and a once-a-second heartbeat from the system
  (`accRun`) backs it up.
- A Control call with csCode 65 (`accRun`) from a program now returns 0; it returned -4 in 1.0.
- A TCP connection to the Mac's own address can complete its steps in a different order from MacTCP's (ApiProbe
  case 703). This is timing only; the connection works.
- Resident memory stays below MacTCP's: in the emulator, 171,816 bytes over the no-network baseline after network
  use (MoofTCP Lite 166,852; MacTCP 195,046).
- The prefs reader and the name resolver are built for size; they run only at start-up and for name lookups.

### Diagnostic tools

- `MoofDiag`: a `stack:` line with the installed version, a `timer:` line (lost timer shots, the largest overdue
  time, heartbeat count), a line that says whether MoofPort was switched to `steadyack`, and a revision word in the
  driver's Status block so older MoofDiags keep working. It can also run unattended (output `MoofDiag-<id>.out`).
- The installer can run unattended from a `MoofTCPInstall.cfg` file, for test harnesses. Run by hand it works as
  before.
- `scripts/release.sh` refuses to build a release with the perf or trace build switches set.

### Tested

- On a real Mac Classic with the MoofPort driver (sessions conf3 and conf3b, 10 blocks each) and with the DaynaPORT
  driver (session m1rc, 2 blocks).
- In the Snow emulator: the installer (install, switch to MacTCP and back, unattended), MultiFinder runs with two
  applications (full and Lite), ApiProbe against MacTCP's recorded answers, and the resident-memory check.
- 47 host tests (45 for the Lite configuration), with AddressSanitizer and UBSan.

### Going back to 1.0

The 1.0 Installer does not replace a newer MoofTCP. To go back, click Remove in the 1.1rc1 Installer first, then
Install in the 1.0 Installer, and restart. Your `MoofTCP Prefs` settings are kept.

### Known limitations

- The timer heartbeat runs only while some program calls `SystemTask`. A program that waits in a synchronous
  MacTCP call on a quiet network calls nothing, so a lost timer task then waits for the next packet or call.
- The limits of 1.0 below still apply.

### Network setup note: AppleShare at start-up

AppleShare volumes set to open at start-up can fail to mount. At start-up AppleShare finds the server with
AppleTalk multicast lookups (4 tries in about 2 seconds, no retry), and on Wi-Fi these are lost far more often than
normal traffic. Measured on the test setup: with the server's Wi-Fi power saving on, about 1 in 7 to 1 in 3 boots
failed; with it off, 0 of 15 at night but up to 1 in 3 at busy times of day. The stock MacTCP + DaynaPORT driver
fails the same way, and in an interleaved test MacTCP + MoofPort failed 4 of 10 boots and MoofTCP 1.1 + MoofPort 1
of 10. It is not caused by MoofTCP or MoofPort. Workaround: open the volume from the Chooser; to make it rarer,
reduce multicast loss (a wired connection for the server, the server's Wi-Fi power saving off).

## 1.0 (first release)

First public release of MoofTCP, a drop-in replacement for MacTCP on 68k Macs running System 6.

### What's in it

- The MacTCP `.IPP` driver interface (TCP, UDP, ICMP echo) and the `dnrp` name resolver, so MacTCP programs
  run unchanged.
- A new TCP/IP stack in portable C99 (`core/`): no `malloc`, no global state, host-tested with sanitizers and
  fuzzers.
- A control panel (address, mask, router, DNS, domain, DHCP checkbox, optional diagnostic log) and a status line.
- A DHCP client (lease, renewal, rebinding).
- **MoofTCP Lite**, a build without DHCP that saves about 5 KB of system memory. The installer carries both
  builds and installs the one you pick with a checkbox.
- An installer that moves MacTCP's files aside, installs MoofTCP, and removes it again putting MacTCP back
  exactly as it was. Interrupted installs repair themselves. Switching between MoofTCP and MoofTCP Lite is one
  click, no Remove needed.
- MultiFinder cleanup: applications that quit with connections open are cleaned up after.
- Diagnostic tools (a separate download): `MoofDiag`, `HeapInfo`, `HeapInfoOpen`, `HeapInfoDNR`, `ApiProbe`
  and `KeyLat`.

### Tested

- On a real Mac Classic (System 6.0.8, 4 MB, BlueSCSI v2 WiFi with DaynaPORT): static address and DHCP,
  telnet with Flynn, installing, switching variants and removing, with no retransmission or transmit errors in
  the diagnostics.
- In the Snow emulator: Flynn, Wikipedia Reader, MacTCP Watcher, NCSA Telnet, Fetch and TurboGopher.
- 40 host tests (38 for the Lite configuration), including fuzzers, with AddressSanitizer and UBSan.

### Known limitations

- No IPv6.
- Tested only with the DaynaPORT Ethernet driver on System 6.0.8.
- DHCP has no lease RELEASE, no INIT-REBOOT and no address-conflict probe. While it runs, programs cannot use
  UDP port 68. With no DHCP server, the first open waits about 10 seconds and then fails.
- Under MultiFinder, free system heap varies more because MultiFinder grows it on demand.
