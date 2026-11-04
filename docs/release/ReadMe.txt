MoofTCP 1.0rc3
==============

MoofTCP is a replacement for MacTCP. Programs that use MacTCP
should work with it unchanged. It keeps its settings in its own
file and does not need MacTCP's.

This is a release candidate. It was developed and tested in an
emulator of a Mac Classic with 4 MB; testing on real hardware
is in progress. Please report problems.

Tested with MoofTCP in the emulator: Flynn, Wikipedia Reader,
MacTCP Watcher, NCSA Telnet, Fetch and TurboGopher (they
connect and work). Eudora starts; its mail was not tried.


TWO VERSIONS: MOOFTCP AND MOOFTCP LITE

The Installer holds two versions of MoofTCP and installs the
one you pick with its box "Lite version (no DHCP)":

  - Box off (the normal choice): MoofTCP. It can get its
    address from a DHCP server (your router).
  - Box ticked: MoofTCP Lite (no DHCP). It saves about 5 KB of
    system memory. Choose it if you set a fixed IP address in
    the MoofTCP control panel. Leave it off if your network
    hands out addresses automatically (DHCP).

Both work the same way otherwise and keep the same settings
file. To switch from one to the other, open the Installer, set
the box the other way, click Install and restart; no Remove is
needed. (When MoofTCP Lite is installed, the box starts ticked.)
MoofTCP Lite uses the address saved in the settings even if
"Get the address by DHCP" was ticked in the other version. Its
control panel says "MoofTCP Lite does not support DHCP."; to
switch back, reinstall without the Lite option.


WHAT YOU NEED

  - A Mac with System 6.0.8 (the only version tested).
  - An Ethernet adapter on the SCSI bus that uses the DaynaPORT
    driver (a BlueSCSI with DaynaPORT, for example). It is the
    only adapter MoofTCP was tested with.
  - Nothing else. You do not need MacTCP's settings; MoofTCP
    has its own.


INSTALL

  1. Open "MoofTCP Installer", leave the box "Lite version (no
     DHCP)" off (or tick it for MoofTCP Lite), and click Install.
  2. Restart the Mac.
  3. Open the Control Panel (Apple menu), then "MoofTCP". Fill in
     your IP address, subnet mask, router, DNS server and
     domain, and save. Restart again. (MoofTCP, not Lite: you can
     tick "Get the address by DHCP" instead of typing an address.
     MoofTCP Lite says "MoofTCP Lite does not support DHCP." in
     that place.)

Until you save an address, MoofTCP stays "not configured" and
programs see that no network is set up. It needs no default
settings.

The Installer moves your MacTCP files into a folder called
"MacTCP (Disabled)" in the System Folder and puts the MoofTCP
file in the System Folder.


REMOVE

  1. Open "MoofTCP Installer" and click Remove (the same for
     both versions).
  2. Restart the Mac.

Remove deletes MoofTCP and puts your MacTCP files back where
they were. They come back exactly as they were when you
installed (MacTCP itself updates its own files every time it
starts, so they may differ from before only in that way).


WARNING

Never copy the "MoofTCP" file into the System Folder by hand next
to MacTCP. It wastes about 190 KB of system memory and MacTCP
stays active, so MoofTCP does nothing. Always use the Installer.


THINGS YOU MAY NOTICE

  - The Finder may show a wrong icon on the parked MacTCP file
    until you rebuild the Desktop (hold Command and Option while
    the Mac restarts).
  - If an install is interrupted (power loss, crash), there can
    be an empty "MacTCP (Disabled)" folder and no network. Run
    the Installer again; it repairs itself.


[BEGIN DIAGNOSTIC LOG TEXT: shipped only if DIAGLOG_TEXT=1 in scripts/release.conf]
REPORTING PROBLEMS

MoofTCP keeps a small diagnostic log of what it did (connections
opening and closing, retransmissions, dropped packets, failed
name lookups). It costs almost nothing and is on by default for
new settings. The switch is the "Keep a diagnostic log" box in
the MoofTCP control panel, under the five fields; a change takes
effect after you restart.

If a program misbehaves (a connection hangs, a transfer stops, a
name does not resolve) but the Mac keeps running:

  1. Right after the problem, without restarting, run MoofDiag.
     It is one of the release-candidate test files; ask for it
     if you do not have it.
  2. MoofDiag writes a text file "MoofDiag.out" next to itself.
  3. Send that file with a short description: the program, what
     you did, and what happened.

The log lives in memory only. A crash or a restart erases it, so
after a crash it cannot help: then please send a photo of the
screen (or the MacsBug text, if you use MacsBug) and describe
what you were doing.
[END DIAGNOSTIC LOG TEXT]


KNOWN LIMITS

  - MoofTCP Lite has no DHCP: you must enter the address yourself.
  - No IPv6.
  - Tested only with the DaynaPORT Ethernet driver.
