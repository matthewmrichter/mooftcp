/*
 * HelloInit: a do-nothing System 6 INIT. At startup INIT 31 calls _start,
 * which draws ICN# 128 in the INIT parade along the bottom of the screen and
 * returns. It installs nothing.
 *
 * Parade position uses the ShowINIT convention (low memory, the tail of
 * CurApName, which is unused at INIT time):
 *   0x092C  short  h of the next icon's left edge
 *   0x092E  short  checksum = rotate-left-1(h) ^ 0x1021
 * A valid checksum means an earlier INIT already placed an icon; otherwise
 * start at the first slot. Each icon advances h by 40.
 *
 * QuickDraw is not set up for us at INIT time, so we build a private
 * QuickDraw world on the stack: point A5 at a longword that InitGraf fills
 * with &qd.thePort, open a port on the screen, draw, then restore A5.
 */
#include <Quickdraw.h>
#include <Resources.h>
#include <OSUtils.h>
#include <Memory.h>
#include <Retro68Runtime.h>

/* low-memory globals live at fixed small addresses; GCC would flag them as null derefs */
#pragma GCC diagnostic ignored "-Warray-bounds"
#define kParadeH     (*(short *)0x092C)
#define kParadeCheck (*(short *)0x092E)

enum {
    kFirstX = 8,      /* left edge of the first icon */
    kBottomGap = 8,   /* icon bottom this far above the screen bottom */
    kIconSize = 32,
    kAdvance = 40,
    kCheckConst = 0x1021,
    kIconID = 128
};

static short ParadeChecksum(short h)
{
    unsigned short u = (unsigned short)h;
    return (short)(((u << 1) | (u >> 15)) ^ kCheckConst);
}

static void ShowParadeIcon(short iconID)
{
    struct {
        QDGlobals qd;
        long a5slot;    /* A5 points here; InitGraf stores &qd.thePort in it */
    } world;
    GrafPort port;
    long savedA5;
    Handle icn;
    BitMap bits;
    Rect dst;
    short h;

    icn = GetResource('ICN#', iconID);
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
    bits.baseAddr = *icn + 128;                 /* mask: clear the area */
    CopyBits(&bits, &port.portBits, &bits.bounds, &dst, srcBic, NULL);
    bits.baseAddr = *icn;                       /* image: OR it in */
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
    RETRO68_RELOCATE();
    Retro68CallConstructors();
    ShowParadeIcon(kIconID);
    Retro68FreeGlobals();
}
