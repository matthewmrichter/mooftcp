MoofTCP 1.0rc1
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


WHAT YOU NEED

  - A Mac with System 6.0.8 (the only version tested).
  - An Ethernet adapter on the SCSI bus that uses the DaynaPORT
    driver (a BlueSCSI with DaynaPORT, for example). It is the
    only adapter MoofTCP was tested with.
  - Nothing else. You do not need MacTCP's settings; MoofTCP
    has its own.


INSTALL

  1. Open "MoofTCP Installer" and click Install.
  2. Restart the Mac.
  3. Open the Control Panel (Apple menu), then "MoofTCP". Fill in
     your IP address, subnet mask, router, DNS server and
     domain, and save. Restart again.

Until you save an address, MoofTCP stays "not configured" and
programs see that no network is set up. It needs no default
settings.

The Installer moves your MacTCP files into a folder called
"MacTCP (Disabled)" in the System Folder and puts the MoofTCP
file in the System Folder.


REMOVE

  1. Open "MoofTCP Installer" and click Remove.
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


REPORTING PROBLEMS

[BEGIN DIAGNOSTIC LOG TEXT: edit with the control panel checkbox]
MoofTCP can keep a diagnostic log of what it did. It is off by
default. To turn it on, open the MoofTCP control panel, tick
"Keep a diagnostic log", save, and restart. Include the log
with your report, together with the program you used and what
you did.
[END DIAGNOSTIC LOG TEXT]


KNOWN LIMITS

  - No DHCP yet: you must enter the address yourself.
  - No IPv6.
  - Tested only with the DaynaPORT Ethernet driver.
