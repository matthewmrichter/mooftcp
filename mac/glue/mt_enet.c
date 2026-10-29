/* mt_enet.c - .ENET0 (DaynaPORT) open / attach / async write / close. See mt_enet.h. */
#include <Devices.h>
#include <stddef.h>
#include <string.h>
#include "mt_enet.h"

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

    zero(e, sizeof *e);
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
    MtEnetTx *s = 0;
    unsigned i;
    uint16_t n;
    int16_t r;

    if (!e->opened) { e->tx_drop_closed++; return 1; }
    if (!mt_enet_len_ok(len)) { e->tx_drop_len++; return 1; }
    for (i = 0; i < MT_ENET_TXSLOTS; i++) {
        MtEnetTx *c = &e->tx[(e->tx_next + i) % MT_ENET_TXSLOTS];
        if (c->pb.ioResult <= 0) {
            if (c->started && c->pb.ioResult < 0) { e->tx_err++; e->last_err = c->pb.ioResult; }
            c->started = 0;
            s = c;
            e->tx_next = (uint8_t)((e->tx_next + i + 1u) % MT_ENET_TXSLOTS);
            break;
        }
    }
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
    e->tx_ok++;
    return 0;
}

int mt_enet_tx_busy(const MtEnet *e)
{
    unsigned i;
    int n = 0;
    for (i = 0; i < MT_ENET_TXSLOTS; i++) if (e->tx[i].pb.ioResult > 0) n++;
    return n;
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
