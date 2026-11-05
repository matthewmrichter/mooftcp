# Security policy

MoofTCP is a network driver for 68k Macs, so a bug in it can be a security bug (for example a crash or memory
corruption triggered by a crafted packet).

## Supported versions

Only the latest release (1.0 at the time of writing) is supported.

## Reporting a vulnerability

Please do **not** open a public issue for a security problem. Use GitHub's private reporting instead:
**Security tab, "Report a vulnerability"** on this repository. Include what you saw, the packet or program that
triggers it if you know it, and your Mac, System version and Ethernet adapter. A `MoofDiag.out` file helps.

This is a hobby project run by one person, so there is no guaranteed response time, but reports are read and taken
seriously.

## Scope

In scope: the TCP/IP stack (`core/`), the driver and resolver (`mac/`), the DHCP client and the installer.
Out of scope: bugs in the Ethernet driver (DaynaPORT, BlueSCSI), in Apple's System software, or in third-party
programs that use MoofTCP.
