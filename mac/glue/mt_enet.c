/* mt_enet.c - .ENET0 (DaynaPORT) open / attach / async write / close. See mt_enet.h. */
#include <Devices.h>
#include <stddef.h>
#include <string.h>
#include "mt_enet.h"
/* mt_os.h cannot be included next to the Toolbox headers (duplicate enumerators): declare the one service */
int16_t mt_os_enet_kill(int16_t refnum, void *pb);
int mt_os_enet_blocked(int16_t refnum, const void *lo, const void *hi);

enum { E_ATTACH = 247, E_DETACH = 248, E_WRITE = 249, E_GETINFO = 252 };

extern void mt_enet_ph(void);
extern void mt_enet_ph_set_ctx(void *gate);

/* Layout the handler stub's .equ lines assume (68k only: the host has other pointer sizes). */
#ifdef __mc68000__
_Static_assert(offsetof(MtGate, head) == 0 && offsetof(MtGate, tail) == 2 &&
               offsetof(MtGate, drops) == 4 && offsetof(MtGate, slot) == 8, "MtGate layout");
_Static_assert(sizeof(MtRxSlot) == 1524 && offsetof(MtRxSlot, buf) == 4, "MtRxSlot layout");
_Static_assert(MT_GATE_SLOTS == 8 && MT_GATE_FRAME == 1520, "stub constants");
_Static_assert(sizeof(MtEnetPB) == 58 && offsetof(MtEnetPB, csCode) == 26 &&
               offsetof(MtEnetPB, eProtType) == 28 && offsetof(MtEnetPB, ePointer) == 30 &&
               offsetof(MtEnetPB, eBuffSize) == 34, "EParamBlock layout");
#endif

static void zero(void *p, size_t n)
{
    uint8_t *b = (uint8_t *)p;
    while (n--) *b++ = 0;
}

static int16_t ctl_sync(MtEnet *e, MtEnetPB *pb)
{
    pb->ioRefNum = e->refnum;
    pb->ioCompletion = 0;
    return (int16_t)PBControlSync((ParmBlkPtr)pb);
}

static int16_t attach(MtEnet *e, uint16_t type)
{
    MtEnetPB pb;
    zero(&pb, sizeof pb);
    pb.csCode = E_ATTACH;
    pb.eProtType = (int16_t)type;
    pb.ePointer = (uint32_t)(uintptr_t)mt_enet_ph;
    return ctl_sync(e, &pb);
}

static void detach(MtEnet *e, uint16_t type)
{
    MtEnetPB pb;
    zero(&pb, sizeof pb);
    pb.csCode = E_DETACH;
    pb.eProtType = (int16_t)type;
    (void)ctl_sync(e, &pb);
}

int mt_enet_open(MtEnet *e, MtGate *gate)
{
    static const unsigned char *const names[2] = { (const unsigned char *)"\p.ENET0",
                                                   (const unsigned char *)"\p.ENET" };
    MtEnetPB pb;
    uint8_t info[18];
    short ref = 0;
    int16_t r = MT_ENET_E_NODRIVER;
    int i;

    if (e->refnum && mt_os_enet_blocked(e->refnum, &e->tx[0], &e->tx[MT_ENET_TXSLOTS]))
        return MT_ENET_E_BUSY;                   /* a re-open behind our stuck PB would hang */
    mt_enet_link_reset(e);                       /* keeps tx[]: the driver may still own PBs there */
    for (i = 0; i < 2 && r != 0; i++) r = (int16_t)OpenDriver(names[i], &ref);
    if (r) { e->last_err = r; return r; }
    e->refnum = ref;
    e->gate = gate;

    /* EGetInfo: the first six bytes returned are our Ethernet address (obs). */
    zero(&pb, sizeof pb);
    zero(info, sizeof info);
    pb.csCode = E_GETINFO;
    pb.ePointer = (uint32_t)(uintptr_t)info;
    pb.eBuffSize = (int16_t)sizeof info;
    r = ctl_sync(e, &pb);
    if (r) { e->last_err = r; return r; }
    for (i = 0; i < 6; i++) e->mac[i] = info[i];
    if ((e->mac[0] & 1) || !(e->mac[0] | e->mac[1] | e->mac[2] | e->mac[3] | e->mac[4] | e->mac[5])) {
        e->last_err = MT_ENET_E_BADMAC;
        return MT_ENET_E_BADMAC;
    }

    mt_enet_ph_set_ctx(gate);
    gate->open = 1;          /* a failed earlier open closed it (mt_enet_close); final review fix 4 */
    r = attach(e, 0x0806);
    if (r == 0) {
        e->attached |= 1;
        r = attach(e, 0x0800);
        if (r == 0) e->attached |= 2;
    }
    if (r) {
        e->last_err = r;
        mt_enet_close(e);
        return r;
    }
    e->opened = 1;
    return 0;
}

int mt_enet_write(void *ctx, const uint8_t *frame, uint16_t len)
{
    MtEnet *e = (MtEnet *)ctx;
    MtEnetTx *s;
    uint16_t n;
    int16_t r;

    if (!e->opened) { e->tx_drop_closed++; return 1; }
    if (!mt_enet_len_ok(len)) { e->tx_drop_len++; return 1; }
    s = mt_enet_tx_claim(e);
    if (!s) { e->tx_drop_busy++; return 1; }

    memcpy(s->frame, frame, len);
    n = mt_enet_pad(s->frame, len);
    if (n != len) e->tx_pad++;
    zero(&s->pb, sizeof s->pb);
    s->wds[0].len = n;
    s->wds[0].ptr = (uint32_t)(uintptr_t)s->frame;
    s->wds[1].len = 0;
    s->wds[1].ptr = 0;
    s->pb.csCode = E_WRITE;
    s->pb.ioRefNum = e->refnum;
    s->pb.ePointer = (uint32_t)(uintptr_t)s->wds;
    r = (int16_t)PBControlAsync((ParmBlkPtr)&s->pb);
    if (r < 0) {
        s->pb.ioResult = r;      /* refused at once: the slot is free again, counted here */
        e->last_err = r;
        e->tx_err++;
        return 1;
    }
    s->started = 1;
    if (!e->tx_started++) e->wd_fresh = 1;
    e->tx_ok++;
    return 0;
}

/* mt_os.h: KillIO for the stuck write (the driver completes it with an error, or does not). */
int16_t mt_os_enet_kill(int16_t refnum, void *pb)
{
    MtEnetPB k;
    (void)pb;
    zero(&k, sizeof k);
    k.ioRefNum = refnum;
    return (int16_t)PBKillIOSync((ParmBlkPtr)&k);
}

#pragma GCC diagnostic ignored "-Warray-bounds"   /* low-memory globals */
/* The DCE of .ENET0: is one of our transmit PBs the head (active) request of its queue? */
int mt_os_enet_blocked(int16_t refnum, const void *lo, const void *hi)
{
    /* UTableBase (0x11C) -> DCtlHandle[~refnum]; DCE: dCtlQHdr at 6 = qFlags(2) qHead(4) qTail(4) */
    const uint8_t *const *const *tab = *(const uint8_t *const *const *const *)0x11C;
    int16_t unit = (int16_t)~refnum;
    const uint8_t *const *h;
    const uint8_t *dce;
    uint32_t head, l = (uint32_t)(uintptr_t)lo & 0x00FFFFFFu, u = (uint32_t)(uintptr_t)hi & 0x00FFFFFFu;
    if (!tab || unit < 0 || unit >= *(volatile const int16_t *)0x1D2) return 0;
    h = (const uint8_t *const *)tab[unit];
    if (!h || !(dce = *h)) return 0;
    head = ((uint32_t)dce[8] << 24 | (uint32_t)dce[9] << 16 | (uint32_t)dce[10] << 8 | dce[11]) & 0x00FFFFFFu;
    return head && head >= l && head < u;
}

/* Detach both handlers, attach both again (task time, gate owned by the caller). No CloseDriver /
 * OpenDriver. The transmit slots are not touched. Ruling (gate consistency): the handler context
 * and gate->open follow the handlers: ctx 0 while none is attached, gate stays open so the tick
 * keeps running the core's timers; a failure leaves opened = 0 (tx_drop_closed). */
int mt_enet_reattach_io(MtEnet *e)
{
    int16_t r;
    if (!e->gate) return MT_ENET_E_NODRIVER;
    if (e->attached & 2) detach(e, 0x0800);
    if (e->attached & 1) detach(e, 0x0806);
    e->attached = 0;
    e->opened = 0;
    mt_enet_ph_set_ctx(e->gate);
    r = attach(e, 0x0806);
    if (r == 0) {
        e->attached |= 1;
        r = attach(e, 0x0800);
        if (r == 0) e->attached |= 2;
    }
    if (r) {
        if (e->attached & 2) detach(e, 0x0800);
        if (e->attached & 1) detach(e, 0x0806);
        e->attached = 0;
        mt_enet_ph_set_ctx(0);
        return r;
    }
    e->opened = 1;
    return 0;
}

void mt_enet_close(MtEnet *e)
{
    if (e->attached & 2) detach(e, 0x0800);
    if (e->attached & 1) detach(e, 0x0806);
    e->attached = 0;
    e->opened = 0;
    mt_enet_ph_set_ctx(0);
    if (e->gate) e->gate->open = 0;
}
