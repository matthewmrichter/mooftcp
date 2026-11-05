# Hardware session {{SESSION}} ({{DATE}})

Fill in the *Report back* section and send it back (with photos).

## Copy

Put these files in the BlueSCSI **shared folder** on the SD card. Use the `.bin` form
first; use the `.hqx` form only if the `.bin` arrives as a plain file with no icon.
Also copy `README.txt` (it opens in TeachText).

{{FILES}}

## Install

{{INSTALL}}

## Try

{{TRY}}

If anything crashes or hangs, stop there, note the step number, and go to *Report back*.

## Report back

- [ ] Every step in *Try* matched the expected result
- [ ] A step failed (note which one): ______
- [ ] A bomb or MacsBug screen appeared
- [ ] The `.bin` files arrived intact (icon and type correct)
- [ ] The `.hqx` files arrived intact (expanded to a working file)
- [ ] I used only the `.bin` files, so I could not compare

What happened, in order:



- Photos taken (screen, error dialogs): ______
- MacsBug message or bomb ID, if any: ______
- Transfer format that arrived intact (`.bin` / `.hqx` / neither): ______
- DaynaPORT driver version: ______
- MacTCP version (Get Info on the MacTCP file in the System Folder): ______
- Memory, from *About the Macintosh* (Total, System, Finder or Largest Unused) and from
  HeapInfo, if it is part of this session:
  - Right after boot: ______
  - After an application has opened the network driver (HeapInfoOpen): ______
  - Name resolver (HeapInfoDNR, second run; both alerts: err/addr, and system/app heap taken
    at open / after lookup / after close): ______

## Undo

Remove everything this session added, then restart:

{{UNDO}}

The Mac should then be exactly as it was before the session.
