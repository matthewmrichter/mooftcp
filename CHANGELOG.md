# Changelog

## 1.1rc1 (release candidate, 2026-11-13)

A faster MoofTCP. The programs, settings file and installer work as in 1.0.

### Faster

Measured on a real Mac Classic (System 6.0.8, 4 MB, BlueSCSI v2 Wi-Fi with MoofPort) on 2026-11-13: MoofTCP 1.1
with MoofPort 1.1 against MoofTCP 1.0 with MoofPort 1.0, 10 paired blocks in one session, timed on the PC. Medians:

| | 1.0 | 1.1 |
|---|---|---|
| Echo round trip, 1 byte | 53.6 ms | about 44.5 ms |
| Terminal burst, 2,000 bytes | 153.6 ms | about 131 ms |
| TCP receive, 1 MB | 45.1 KB/s | about 54.8 KB/s |
| TCP send, 4 sends queued | 38.0 KB/s | about 41.0 KB/s |

These four were better in all 10 pairs. One-at-a-time sends and AppleShare copies showed no significant
difference. The figures were measured over Wi-Fi and vary with the network.

### Changed

- A shorter path through every MacTCP call and every packet: less work per call, per timer tick and per frame
  (the speed-up above).
- When the Ethernet driver is MoofPort 1.1, MoofTCP switches it to MoofPort's `steadyack` timing when it opens the
  driver. With any other driver (DaynaPORT, for example) it changes nothing.
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

### Known limitations

- The timer heartbeat runs only while some program calls `SystemTask`. A program that waits in a synchronous
  MacTCP call on a quiet network calls nothing, so a lost timer task then waits for the next packet or call.
- AppleShare volumes set to open at start-up can fail to mount when the AppleShare server is itself on Wi-Fi with
  Wi-Fi power saving on. AppleTalk's start-up lookups are multicast, and a power-saving Wi-Fi receiver misses most
  of them (about 1 in 7 boots with power saving on, 0 in 15 with it off; the stock DaynaPORT driver too). Turn
  power saving off on the server, use a wired server, or open the volume from the Chooser.
- The limits of 1.0 below still apply.

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
