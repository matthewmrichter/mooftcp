/* moof.r - the MoofTCP control panel file "MoofTCP" (type cdev, creator ztcp: the real MacTCP
 * file's, docs/dev/MACTCP-INTERFACE.md "File and resource layout"; Plan 3 Task 8 renamed it from
 * the interim "MacTCP" and added the control panel).
 *
 *   INIT 0     sysheap, locked   boot loader (mac/init/mt_init.c; backs off if .ipp exists)
 *   DRVR 22    ".ipp", sysheap, locked   the driver: header + stubs here, code from MoofDrvr.flt
 *   dnrp 0     "DNR", locked     the resolver (dnrp.r)
 *   sysz 0     system-heap bytes the INIT needs (measured, MACTCP-INTERFACE.md "sysz")
 *   ICN# 128   parade icon
 *   the control panel: cdev/mach/nrct/DITL -4064, alerts, BNDL/FREF, icons, vers 1/2
 *              (mac/cdev/moofcdev.r)
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

/* System-heap bytes INIT 31 adds for us: 0x38000 = 229,376 (RC3: the DHCP client adds about 5.3 KB of resident driver code, which would have left about 15.3 KB free after the open on the real Mac, one step over the 15K floor; 0x34000 = 212,992 was RC2: RC1 hardware, single Finder: 18,628 free at boot, 4,234 after
 * the first open, the open costs 14,394; +0x4000 gives about 20.6K after the open, over the 15K floor. Before: 0x30000, the final re-review raised it from 0x2C000:
 * DRVR 79.8K and the resident block 59.1K left 14.5K free at the soak's worst step, under the 15K floor;
 * Task 11 had raised 0x28000 to 0x2C000 when the DNR grew the DRVR to about 78K). The DRVR code,
 * the resident block (gate, Ethernet slots, MtDrv, core arena), the DCE, plus
 * the .ENET0 driver the first open loads (about 16.5K); 0x24000 booted but left too little (-23000).
 * Derivation: docs/dev/MACTCP-INTERFACE.md "MoofTCP driver (Task 10)". */
#ifdef MT_NO_DHCP
/* MoofTCP Lite (no DHCP client, about 5.3 KB less resident): the RC2 value 0x34000, decided from the
 * RC1 hardware HeapInfo numbers before DHCP existed (above). */
data 'sysz' (0) {
    $"0003 4000"
};
#else
data 'sysz' (0) {
    $"0003 8000"
};
#endif

/* Startup icon "Moof Stamp" (Moof icon concepts, round 2): 128 bytes of image, then 128 bytes of mask. */
data 'ICN#' (128) {
    $"2AAAAAA8 7FFFFFFE C0000003 5FFFFFFA"
    $"D000000B 5020040A D0300C0B 501FF80A"
    $"D0E0070B 51E0078A D3EE07CB 53EA37CA"
    $"D3CE33CB 53C003CA D1C7E38B 50C8130A"
    $"D053CA0B 5013C80A D010880B 5009500A"
    $"D007E00B 5000000A D22633CB 53694A0A"
    $"D2A94B8B 52294A0A D226320B 5000000A"
    $"DFFFFFFB 40000002 7FFFFFFE 2AAAAAA8"
    /* mask */
    $"2AAAAAA8 7FFFFFFE FFFFFFFF 7FFFFFFE"
    $"FFFFFFFF 7FFFFFFE FFFFFFFF 7FFFFFFE"
    $"FFFFFFFF 7FFFFFFE FFFFFFFF 7FFFFFFE"
    $"FFFFFFFF 7FFFFFFE FFFFFFFF 7FFFFFFE"
    $"FFFFFFFF 7FFFFFFE FFFFFFFF 7FFFFFFE"
    $"FFFFFFFF 7FFFFFFE FFFFFFFF 7FFFFFFE"
    $"FFFFFFFF 7FFFFFFE FFFFFFFF 7FFFFFFE"
    $"FFFFFFFF 7FFFFFFE 7FFFFFFE 2AAAAAA8"
};

#include "../cdev/moofcdev.r"
