/* moof.r - the interim MoofTCP cdev file "MacTCP" (type cdev, creator ztcp: the real MacTCP
 * file's, docs/dev/MACTCP-INTERFACE.md "File and resource layout"). Task 11 adds 'dnrp' 0.
 *
 *   INIT 0     sysheap, locked   boot loader (mac/init/mt_init.c)
 *   DRVR 22    ".ipp", sysheap, locked   the driver: header + stubs here, code from MoofDrvr.flt
 *   sysz 0     system-heap bytes the INIT needs (measured, MACTCP-INTERFACE.md "sysz")
 *   vers 1, ICN# 128 (parade icon)
 */
#include "Retro68.r"
#include "MacTypes.r"

type 'INIT' {
    RETRO68_CODE_TYPE
};

resource 'INIT' (0, sysheap, locked) {
    dontBreakAtEntry, $$read("MoofInit.flt");
};

/* DRVR: the 18-byte header (flags 0x5C00 = dNeedLock | dNeedGoodBye | dStatEnable |
 * dCtlEnable, as MacTCP's; delay, event mask, menu 0; entry offsets), the name ".ipp" (MacTCP's
 * exact name; the Device Manager compares names case-insensitively) padded to offset 24, then one
 * stub per entry: moveq #selector,d0 / bra.s 48. Offset 44 is the boot stub the INIT calls
 * (moveq #5,d0 / nop) and offset 48 the flat code (its first instruction is Retro68's entry
 * trampoline, which jumps to _start in mt_drv_stubs.s). */
type 'DRVR' {
    hex string;     /* header and stubs */
    hex string;     /* flat code */
};

resource 'DRVR' (22, ".ipp", sysheap, locked) {
    $"5C00 0000 0000 0000"          /* flags, delay, emask, menu */
    $"0018 001C 0020 0024 0028"     /* open, prime, control, status, close */
    $"042E 6970 7000"               /* "\p.ipp", pad */
    $"7000 6014"                    /* 24 open:    moveq #0,d0; bra.s 48 */
    $"7001 6010"                    /* 28 prime:   moveq #1,d0; bra.s 48 */
    $"7002 600C"                    /* 32 control: moveq #2,d0; bra.s 48 */
    $"7003 6008"                    /* 36 status:  moveq #3,d0; bra.s 48 */
    $"7004 6004"                    /* 40 close:   moveq #4,d0; bra.s 48 */
    $"7005 4E71",                   /* 44 boot:    moveq #5,d0; nop (falls into 48) */
    $$read("MoofDrvr.flt")
};

/* The resolver, `dnrp` 0 (Task 11, mac/dnr): apps load it from the file "MacTCP DNR" (dnrfile.r
 * builds that file with the same resource); the cdev carries a copy as the real MacTCP file does. */
#include "dnrp.r"

/* System-heap bytes INIT 31 adds for us: 0x30000 = 196,608 (the final re-review raised it from 0x2C000:
 * DRVR 79.8K and the resident block 59.1K left 14.5K free at the soak's worst step, under the 15K floor;
 * Task 11 had raised 0x28000 to 0x2C000 when the DNR grew the DRVR to about 78K). The DRVR code,
 * the resident block (gate, Ethernet slots, MtDrv, core arena), the DCE, plus
 * the .ENET0 driver the first open loads (about 16.5K); 0x24000 booted but left too little (-23000).
 * Derivation: docs/dev/MACTCP-INTERFACE.md "MoofTCP driver (Task 10)". */
data 'sysz' (0) {
    $"0003 0000"
};

resource 'vers' (1) {
    0x00, 0x10, development, 0x00, verUS,
    "0.1d0",
    "MoofTCP 0.1d0 (interim, Plan 2)"
};

/* Parade icon: 128 bytes of image, then 128 bytes of mask. */
data 'ICN#' (128) {
    $"00000000 3FFFFFFC 40000002 40000002"
    $"4C0C0002 4E1C0002 4F3C0002 4DEC0002"
    $"4CCC0002 4C0C0002 4C0C0002 4C0C0002"
    $"40000002 400FCFC2 40030C02 40030C02"
    $"40030C02 40030FC2 400300C2 400300C2"
    $"40030FC2 40000002 4F3CF3C2 49249242"
    $"4F3CF3C2 40000002 40000002 3FFFFFFC"
    $"00200400 00FFFC00 00000000 00000000"
    /* mask */
    $"00000000 7FFFFFFE 7FFFFFFE 7FFFFFFE"
    $"7FFFFFFE 7FFFFFFE 7FFFFFFE 7FFFFFFE"
    $"7FFFFFFE 7FFFFFFE 7FFFFFFE 7FFFFFFE"
    $"7FFFFFFE 7FFFFFFE 7FFFFFFE 7FFFFFFE"
    $"7FFFFFFE 7FFFFFFE 7FFFFFFE 7FFFFFFE"
    $"7FFFFFFE 7FFFFFFE 7FFFFFFE 7FFFFFFE"
    $"7FFFFFFE 7FFFFFFE 7FFFFFFE 7FFFFFFE"
    $"00200400 00FFFC00 00000000 00000000"
};
