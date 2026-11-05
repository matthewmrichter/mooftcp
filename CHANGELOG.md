# Changelog

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
