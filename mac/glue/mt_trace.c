/* mt_trace.c - the echo-path trace (PERF BUILDS ONLY: MT_TUNE + MT_TRACE, task 18; see mt_trace.h and
 * mt_trace_abi.h). Plain C on mt_os.h, host-tested (tests/test_trace.c, test_fuzz_pb in build-tune-trace); the Mac
 * clock and the EWrite completion stub are in mt_trace_mac.c. Every other build compiles this file to nothing. */
#ifdef MT_TRACE
#include <string.h>
#include "mt_trace.h"
#include "mt_os.h"

static MtTrace *g_tr;              /* the resident ring (mt_trace_set at boot; no A5: the driver's own code block) */

void mt_trace_set(MtTrace *t) { g_tr = t; }
MtTrace *mt_trace_get(void) { return g_tr; }

/* One record: the clock read and the store under one mask, so a record is never torn and the ring index never
 * raced (the protocol handler, a completion routine or the Time Manager may record over any other context). */
void mt_trace_ev(uint8_t ev, uint8_t a, uint16_t b, uint32_t c, uint32_t d)
{
    MtTrace *t = g_tr;
    MtTraceRec *r;
    int m;
    if (!t || t->frozen) return;
    m = mt_os_mask();
    if (!t->frozen) {
        r = &t->rec[t->next];
        r->t = mt_os_trace_us();
        r->ev = ev;
        r->a = a;
        r->b = b;
        r->c = c;
        r->d = d;
        t->next = (uint16_t)(t->next + 1u >= MT_TRACE_RECS ? 0u : t->next + 1u);
        t->total++;
    }
    mt_os_unmask(m);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

void mt_trace_frame_fields(const uint8_t *f, uint16_t len, uint8_t *a, uint16_t *b, uint32_t *c, uint32_t *d)
{
    uint16_t type, ihl, iplen, doff;
    uint8_t proto = 0;
    const uint8_t *l4;
    *a = MT_TRACE_A_NOTTCP;
    *b = len;
    *c = 0;
    *d = 0;
    if (len < 14) return;
    type = rd16(f + 12);
    *c = (uint32_t)type << 16;
    if (type != 0x0800 || len < 24) return;
    proto = f[23];
    *c |= proto;
    if (len < 34) return;
    ihl = (uint16_t)((f[14] & 15u) * 4u);
    if (ihl < 20 || len < 14u + ihl + 8u) return;
    l4 = f + 14 + ihl;
    *d = (uint32_t)rd16(l4) << 16 | rd16(l4 + 2);
    if (proto != 6 || len < 14u + ihl + 20u) {
        if (proto != 17) *d = 0;
        return;
    }
    doff = (uint16_t)((l4[12] >> 4) * 4u);
    iplen = rd16(f + 16);
    *a = (uint8_t)(l4[13] & MT_TRACE_A_FLAGS);
    *b = iplen > ihl + doff ? (uint16_t)(iplen - ihl - doff) : 0;
    *c = rd32(l4 + 4);
}

void mt_trace_frame(uint8_t ev, int mark, const uint8_t *f, uint16_t len)
{
    uint8_t a;
    uint16_t b;
    uint32_t c, d;
    if (!g_tr || g_tr->frozen) return;
    mt_trace_frame_fields(f, len, &a, &b, &c, &d);
    mt_trace_ev(ev, (uint8_t)(a | (mark ? MT_TRACE_A_MARK : 0)), b, c, d);
}

void mt_trace_gate_rx(MtGate *g, int deferred)
{
    MtRxSlot *s = &g->slot[(g->head + MT_GATE_SLOTS - 1u) % MT_GATE_SLOTS];   /* the frame just committed */
    s->pad = (uint16_t)(deferred != 0);          /* MtRxSlot.pad: the stub never writes it */
    mt_trace_frame(MT_TRACE_RX, deferred, s->buf, s->len);
}

void mt_trace_gate_run(MtRxSlot *s)
{
    if (!s->pad) return;
    s->pad = 0;
    mt_trace_frame(MT_TRACE_RXRUN, 0, s->buf, s->len);
}

void mt_trace_ewrite(MtEnetTx *s, uint16_t n, int pumped)
{
    s->pb.ioCompletion = mt_os_trace_ew_compl();
    mt_trace_frame(MT_TRACE_EWRITE, pumped, s->frame, n);
}

void mt_trace_ew_done(MtEnetPB *pb)
{
    MtEnetTx *s = (MtEnetTx *)(void *)pb;        /* the PB is the slot's first member */
    mt_trace_frame(MT_TRACE_EWDONE, pb->ioResult != 0, s->frame, s->wds[0].len);
}

static void info(const MtTrace *t, MtTraceBlk *blk)
{
    blk->got = (uint16_t)(t->total < MT_TRACE_RECS ? t->total : MT_TRACE_RECS);
    blk->cap = MT_TRACE_RECS;
    blk->total = t->total;
    blk->lap_us = t->lap_us;
    blk->now_us = mt_os_trace_us();
    blk->rec_bytes = (uint16_t)sizeof(MtTraceRec);
    blk->flags = (uint16_t)(t->flags | (t->frozen ? MT_TRACE_F_FROZEN : 0));
}

int16_t mt_trace_control(MtTraceBlk *blk)
{
    MtTrace *t = g_tr;
    uint32_t kept, oldest, i, n;
    MtTraceRec *buf;
    int m;
    if (!t || !blk || blk->version != MT_TRACE_ABI_VERSION) return MT_TRACE_PARAMERR;
    switch (blk->op) {
    case MT_TRACE_OP_INFO:
        break;
    case MT_TRACE_OP_FREEZE:
        t->frozen = 1;
        break;
    case MT_TRACE_OP_CLEAR:
        m = mt_os_mask();
        t->total = 0;
        t->next = 0;
        t->frozen = 0;
        mt_os_unmask(m);
        break;
    case MT_TRACE_OP_READ:
        buf = (MtTraceRec *)mt_os_ptr(blk->buf);
        if (!t->frozen || !buf) return MT_TRACE_PARAMERR;
        kept = t->total < MT_TRACE_RECS ? t->total : MT_TRACE_RECS;
        oldest = t->total < MT_TRACE_RECS ? 0 : t->next;
        n = blk->first < kept ? kept - blk->first : 0;
        if (n > blk->max) n = blk->max;
        for (i = 0; i < n; i++) memcpy(&buf[i], &t->rec[(oldest + blk->first + i) % MT_TRACE_RECS], sizeof *buf);
        info(t, blk);
        blk->got = (uint16_t)n;
        return 0;
    default:
        return MT_TRACE_PARAMERR;
    }
    info(t, blk);
    return 0;
}
#endif
