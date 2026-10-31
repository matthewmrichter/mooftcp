/* mt_drv_mac.c - Toolbox side of the .IPP driver: see mt_drv_mac.h. */
#include <Memory.h>
#include <OSUtils.h>
#include <Events.h>
#include <Devices.h>
#include <Resources.h>
#include <stddef.h>
#include "mt_drv_mac.h"

/* Low-memory globals live at fixed small addresses; GCC would flag them as null derefs. */
#pragma GCC diagnostic ignored "-Warray-bounds"

#define UTABLE_BASE  (*(uint8_t ***volatile *)0x11C)     /* UTableBase: DCtlHandle[] */
#define UNIT_COUNT   (*(volatile int16_t *)0x1D2)       /* UnitNtryCnt */
#define LM_TICKS     (*(volatile uint32_t *)0x16A)
#define LM_TIME      (*(volatile uint32_t *)0x20C)
#define LM_MOUSE     (*(volatile uint32_t *)0x830)      /* Mouse: v, h */
#define LM_VIA       (*(uint8_t *volatile *)0x1D4)      /* VIA1 base */

/* DCE layout (Inside Macintosh II, Device Manager): offsets used here. */
enum { DCE_DRIVER = 0, DCE_FLAGS = 4, DCE_QHDR = 6, DCE_REFNUM = 24, DCE_DELAY = 34,
       DCE_EMASK = 36, DCE_MENU = 38, DCE_SIZE = 40 };
enum { DRVR_FLAGS = 0, DRVR_DELAY = 2, DRVR_EMASK = 4, DRVR_MENU = 6, DRVR_NAME = 18 };
#define F_RAMBASED 0x0040u
#define F_ACTIVE   0x0080u

static uint32_t strip24(const void *p) { return (uint32_t)(uintptr_t)p & 0x00FFFFFFu; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

typedef uint16_t sr_t;
static inline sr_t irq_off(void) {
    sr_t sr;
    __asm__ volatile("move.w %%sr,%0\n\tor.w #0x0700,%%sr" : "=d"(sr) : : "memory", "cc");
    return sr;
}
static inline void irq_on(sr_t sr) { __asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "memory", "cc"); }

static short g_saved_res;
void *mt_mac_zone_sys(void) {
    THz z = GetZone();
    SetZone(*(THz *)0x2A6);                  /* SysZone */
    g_saved_res = CurResFile();
    UseResFile(*(volatile short *)0xA58);    /* SysMap: the System file */
    return (void *)z;
}
void mt_mac_zone_restore(void *z) {
    UseResFile(g_saved_res);
    SetZone((THz)z);
}

void *mt_mac_alloc_sys(uint32_t n) { return (void *)NewPtrSysClear((Size)n); }
void  mt_mac_free(void *p) { if (p) DisposePtr((Ptr)p); }

static uint8_t lower(uint8_t c) { return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + 32) : c; }

/* The DRVR header of unit table entry i, or NULL. */
static const uint8_t *unit_driver(int16_t i) {
    uint8_t **h = UTABLE_BASE[i];
    const uint8_t *dce, *drv;
    if (!h || !*h) return NULL;
    dce = (const uint8_t *)strip24(*h);
    drv = (const uint8_t *)strip24(*(void *const *)(dce + DCE_DRIVER));
    if (rd16(dce + DCE_FLAGS) & F_RAMBASED) {   /* a handle to the driver */
        if (!drv || !*(void *const *)drv) return NULL;
        drv = (const uint8_t *)strip24(*(void *const *)drv);
    }
    return drv;
}

int mt_mac_unit_find(const uint8_t *pname) {
    int16_t i, n = UNIT_COUNT;
    for (i = 0; i < n; i++) {
        const uint8_t *drv = unit_driver(i), *nm;
        uint8_t k;
        if (!drv) continue;
        nm = drv + DRVR_NAME;
        if (nm[0] != pname[0]) continue;
        for (k = 1; k <= pname[0] && lower(nm[k]) == lower(pname[k]); k++) {}
        if (k > pname[0]) return 1;
    }
    return 0;
}

/* Grow the unit table to `want` entries (a new block in the system heap; the old one is left
 * alone: it may not be a heap block of its own). */
static int grow_units(int16_t want) {
    int16_t n = UNIT_COUNT, i;
    uint8_t ***old = UTABLE_BASE, ***nt;
    sr_t sr;
    nt = (uint8_t ***)NewPtrSysClear((Size)want * 4);
    if (!nt) return -108;
    sr = irq_off();
    for (i = 0; i < n; i++) nt[i] = old[i];
    UTABLE_BASE = nt;
    UNIT_COUNT = want;
    irq_on(sr);
    return 0;
}

int mt_mac_unit_install(void *drvr, int16_t want, int16_t *refnum) {
    const uint8_t *hdr = (const uint8_t *)strip24(drvr);
    Handle h;
    uint8_t *dce;
    int16_t unit;
    if (UNIT_COUNT <= want) {
        int16_t n = (int16_t)((want + 16) & ~15);
        int r = grow_units(n);
        if (r) return r;
    }
    for (unit = want; unit < UNIT_COUNT && UTABLE_BASE[unit]; unit++) {}
    if (unit >= UNIT_COUNT) return -29;               /* unitTblFullErr */
    h = NewHandleSysClear(DCE_SIZE);
    if (!h) return -108;
    HLock(h);
    dce = (uint8_t *)strip24(*h);
    wr32(dce + DCE_DRIVER, strip24(hdr));             /* pointer-based: never locked/moved */
    wr16(dce + DCE_FLAGS, (uint16_t)(rd16(hdr + DRVR_FLAGS) & 0xFF00u));
    wr16(dce + DCE_REFNUM, (uint16_t)~unit);
    wr16(dce + DCE_DELAY, rd16(hdr + DRVR_DELAY));
    wr16(dce + DCE_EMASK, rd16(hdr + DRVR_EMASK));
    wr16(dce + DCE_MENU, rd16(hdr + DRVR_MENU));
    UTABLE_BASE[unit] = (uint8_t **)h;
    *refnum = (int16_t)~unit;
    return 0;
}

void mt_mac_dequeue(void *pb, void *dce) {
    uint8_t *d = (uint8_t *)dce;
    (void)Dequeue((QElemPtr)pb, (QHdrPtr)(d + DCE_QHDR));
    d[DCE_FLAGS + 1] = (uint8_t)(d[DCE_FLAGS + 1] & ~F_ACTIVE);
}

void mt_mac_entropy(uint32_t *a, uint32_t *b) {
    uint32_t t = LM_TICKS, tm = LM_TIME, m = LM_MOUSE, tc = (uint32_t)TickCount();
    const uint8_t *via = LM_VIA;
    uint32_t v = via ? ((uint32_t)via[0x0800] << 8 | via[0x0A00]) : 0;   /* VIA1 T1 counter */
    uint32_t sp = (uint32_t)(uintptr_t)&t;
    *a = t ^ (tm << 7 | tm >> 25) ^ (v << 16) ^ (tc * 0x9E3779B9u);
    *b = m ^ (tm * 2654435761u) ^ (v * 40503u) ^ (sp << 3) ^ (tc << 21);
}
