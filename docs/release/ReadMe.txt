MoofTCP 1.0rc1
==============

MoofTCP is a replacement for MacTCP. Programs that use MacTCP
(Fetch, NCSA Telnet, Eudora, Flynn, and so on) run with it
unchanged. It keeps its settings in its own file and does not
need MacTCP's.

This is a release candidate. Please report problems.


WHAT YOU NEED

  - A Mac with System 6.0.x (made and tested on a Mac Classic
    with 4 MB).
  - An Ethernet adapter on the SCSI bus that uses the DaynaPORT
    driver (a BlueSCSI with DaynaPORT, for example).
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

MoofTCP can keep a diagnostic log of what it did. It is off by
default and is switched on by a flag in the "MoofTCP Prefs" file
(there is no switch for it in the control panel yet). If you were
asked to turn it on, include the log with your report, together
with the program you used and what you did.


KNOWN LIMITS

  - No DHCP yet: you must enter the address yourself.
  - No IPv6.
  - Only the DaynaPORT Ethernet driver is supported.
