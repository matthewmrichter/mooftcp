/* mf_os.c - Toolbox wrappers for MfEcho (see mf_os.h). */
#pragma GCC diagnostic ignored "-Wpointer-sign"
#pragma GCC diagnostic ignored "-Warray-bounds"
#include <Quickdraw.h>
#include <Dialogs.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Memory.h>
#include <Events.h>
#include <OSUtils.h>
#include <Files.h>
#include <Devices.h>
#include <Traps.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "mt.h"
#include "mt_drv.h"
#include "mf_os.h"

QDGlobals qd;
static short outref = -1;

#define LM32(a) (*(volatile uint32_t *)(a))
#define LM16(a) (*(volatile int16_t *)(a))
static uint32_t s24(uint32_t p) { return p & 0x00FFFFFFu; }

static int trap_avail(uint16_t trap)
{
    /* toolbox traps (bit 11) vs OS traps; unimplemented = the address of _Unimplemented (A89F) */
    ProcPtr unimp = NGetTrapAddress(0xA89F, kToolboxTrapType);
    return NGetTrapAddress(trap, (trap & 0x0800) ? kToolboxTrapType : kOSTrapType) != unimp;
}

void os_init(void)
{
    MaxApplZone();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(0);
    InitCursor();
    FlushEvents(everyEvent, 0);
}

uint32_t os_ticks(void) { return (uint32_t)TickCount(); }
short os_open_ipp(short *ref) { return (short)OpenDriver("\p.IPP", ref); }
short os_control_sync(void *pb) { return (short)PBControlSync((ParmBlkPtr)pb); }
uint32_t os_cur_a5(void) { return LM32(0x904); }
uint32_t os_appl_zone(void) { return LM32(0x2AA); }

void os_app_name(char *c, int max)
{
    const unsigned char *p = (const unsigned char *)0x910;   /* CurApName, Str31 */
    int n = p[0] < 31 ? p[0] : 31;
    if (n > max - 1) n = max - 1;
    memcpy(c, p + 1, (size_t)n);
    c[n] = 0;
}

static void pname(unsigned char *ps, const char *suffix)
{
    char c[48];
    os_app_name(c, 32);
    strcat(c, suffix);
    ps[0] = (unsigned char)strlen(c);
    memcpy(ps + 1, c, ps[0]);
}

short os_out_open(void)
{
    Str63 ps;
    OSErr e;
    pname(ps, ".out");
    Create(ps, 0, 'ttxt', 'TEXT');
    e = FSOpen(ps, 0, &outref);
    if (e) { outref = -1; return (short)e; }
    SetEOF(outref, 0);
    return 0;
}

void os_out(const char *fmt, ...)
{
    char b[300];
    va_list ap;
    long n;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    n = (long)strlen(b);
    if (outref >= 0) { FSWrite(outref, &n, b); FlushVol(0, 0); }
}

void os_out_close(void)
{
    if (outref >= 0) { FSClose(outref); FlushVol(0, 0); outref = -1; }
}

int os_read_cfg(char *buf, int max)
{
    Str63 ps;
    short ref;
    long cnt = max;
    pname(ps, ".cfg");
    if (FSOpen(ps, 0, &ref)) return 0;
    if (FSRead(ref, &cnt, buf) && cnt == 0) { FSClose(ref); return 0; }
    FSClose(ref);
    return (int)cnt;
}

int os_yield(uint32_t ticks)
{
    EventRecord ev;
    uint32_t t0 = os_ticks();
    int wne = trap_avail(0xA860);
    do {
        Boolean got;
        if (wne) got = WaitNextEvent(everyEvent, &ev, 6, 0);
        else { SystemTask(); got = GetNextEvent(everyEvent, &ev); }
        if (got && ev.what == keyDown && (ev.modifiers & cmdKey) && (ev.message & charCodeMask) == 'q') return 1;
    } while (os_ticks() - t0 < ticks);
    return 0;
}

void os_dump_lowmem(const char *tag)
{
    char name[40];
    uint32_t az = LM32(0x2AA), bklim = 0;
    const uint8_t *pre = (const uint8_t *)s24(az) - 8;
    if (az) bklim = s24(*(volatile uint32_t *)s24(az));       /* Zone.bkLim is the zone's first field */
    os_app_name(name, sizeof name);
    os_out("%s: ticks %lu app '%s' CurApRefNum %d ApplZone %08lx ApplLimit %08lx HeapEnd %08lx BufPtr %08lx "
           "CurrentA5 %08lx CurStackBase %08lx SysZone %08lx TheZone %08lx MemTop %08lx bkLim %08lx "
           "pre %02x%02x%02x%02x %02x%02x%02x%02x MF %d WNE %d\r",
           tag, (unsigned long)os_ticks(), name, LM16(0x900), (unsigned long)az, (unsigned long)LM32(0x130),
           (unsigned long)LM32(0x114), (unsigned long)LM32(0x10C), (unsigned long)LM32(0x904),
           (unsigned long)LM32(0x908), (unsigned long)LM32(0x2A6), (unsigned long)LM32(0x118),
           (unsigned long)LM32(0x108), (unsigned long)bklim, pre[0], pre[1], pre[2], pre[3], pre[4], pre[5],
           pre[6], pre[7], trap_avail(0xA88F), trap_avail(0xA860));
}

void *os_drv_status(short ref, uint32_t *n_goodbye, uint32_t *n_forced, uint32_t *n_dropped)
{
    CntrlParam pb;
    MtRes *m;
    OSErr r;
    memset(&pb, 0, sizeof pb);
    pb.ioCRefNum = ref;
    pb.csCode = MT_DRV_STATUS_RES;
    r = PBStatusSync((ParmBlkPtr)&pb);
    memcpy(&m, &pb.csParam[0], 4);
    if (r || !m || m->version != MT_STATUS_VERSION) return 0;
    *n_goodbye = m->entry.n_goodbye;
    *n_forced = m->entry.n_goodbye_forced;
    *n_dropped = m->entry.n_pend_dropped;
    return m->d;
}
