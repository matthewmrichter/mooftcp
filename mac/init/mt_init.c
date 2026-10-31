/*
 * mt_init.c - MoofTCP's boot-time loader, resource INIT 0 of the control panel file "MoofTCP"
 * (mac/moof/moof.r). INIT 31 calls it with the cdev file as the current resource file.
 *
 *   0. Back off (Review Focus 2): if the unit table already holds a driver named ".ipp" (the real
 *      MacTCP's INIT ran first, or a second copy of MoofTCP did), do nothing at all: no load, no
 *      icon. A manual copy of MoofTCP next to MacTCP therefore never replaces MacTCP's driver
 *      (mac/include/mt_units.h; the control panel reads the same table for its status line).
 *   1. Get1Resource('DRVR', 22): the driver (sysheap + locked attributes: it loads into the
 *      system heap), detach it from the file, make it unpurgeable and locked: it stays where it is
 *      for the rest of the session (its code is relocated in place, the unit table, the protocol
 *      handler and the Time Manager hold pointers into it).
 *   2. Call its boot stub (offset 44 of the DRVR, see mt_drv_stubs.s): mt_drv_boot allocates the
 *      resident block (NewPtrSys), reads "MoofTCP Prefs", seeds the stack's entropy, opens
 *      .ENET0 and installs the driver in the unit table (refnum -49).
 *   3. Draw the parade icon (ICN# 128) only if all of that worked and an address is configured.
 *      An unconfigured boot stays installed (OpenDriver(".IPP") then fails with -23004, as MacTCP
 *      does) but shows no icon; a failed boot leaves nothing behind.
 *
 * Parade position: the ShowINIT convention (mac/hello_init/init.c has the details).
 */
#include <Quickdraw.h>
#include <Resources.h>
#include <OSUtils.h>
#include <Memory.h>
#include <Retro68Runtime.h>
#include "mt_units.h"

#define kParadeH     (*(short *)0x092C)
#define kParadeCheck (*(short *)0x092E)

#pragma GCC diagnostic ignored "-Warray-bounds"

enum { kFirstX = 8, kBottomGap = 8, kIconSize = 32, kAdvance = 40, kCheckConst = 0x1021,
       kIconID = 128, kDrvrID = 22, kBootStub = 44 };

typedef long (*BootProc)(void *drvr);

static short ParadeChecksum(short h)
{
    unsigned short u = (unsigned short)h;
    return (short)(((u << 1) | (u >> 15)) ^ kCheckConst);
}

static void ShowParadeIcon(short iconID)
{
    struct {
        QDGlobals qd;
        long a5slot;
    } world;
    GrafPort port;
    long savedA5;
    Handle icn;
    BitMap bits;
    Rect dst;
    short h;

    icn = Get1Resource('ICN#', iconID);
    if (icn == NULL)
        return;
    savedA5 = SetA5((long)&world.a5slot);
    InitGraf(&world.qd.thePort);
    OpenPort(&port);
    h = kParadeH;
    if (kParadeCheck != ParadeChecksum(h))
        h = kFirstX;
    SetRect(&dst, h, port.portRect.bottom - kBottomGap - kIconSize,
            h + kIconSize, port.portRect.bottom - kBottomGap);
    HLock(icn);
    bits.rowBytes = 4;
    SetRect(&bits.bounds, 0, 0, kIconSize, kIconSize);
    bits.baseAddr = *icn + 128;
    CopyBits(&bits, &port.portBits, &bits.bounds, &dst, srcBic, NULL);
    bits.baseAddr = *icn;
    CopyBits(&bits, &port.portBits, &bits.bounds, &dst, srcOr, NULL);
    HUnlock(icn);
    ReleaseResource(icn);
    h += kAdvance;
    kParadeH = h;
    kParadeCheck = ParadeChecksum(h);
    ClosePort(&port);
    SetA5(savedA5);
}

void _start(void)
{
    Handle h;
    long r = -1;

    RETRO68_RELOCATE();
    if (mt_units_find_ipp() != NULL) {             /* MacTCP (or another MoofTCP) owns .ipp */
        Retro68FreeGlobals();
        return;
    }
    h = Get1Resource('DRVR', kDrvrID);
    if (h) {
        char *drvr;
        DetachResource(h);
        HNoPurge(h);
        HLock(h);
        drvr = (char *)((unsigned long)*h & 0x00FFFFFFUL);   /* StripAddress24 */
        r = ((BootProc)(drvr + kBootStub))(drvr);
        if (r < 0)
            DisposeHandle(h);
    }
    if (r == 0)
        ShowParadeIcon(kIconID);
    Retro68FreeGlobals();
}
