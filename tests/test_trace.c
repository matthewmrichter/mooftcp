/* test_trace.c - the echo-path trace (mac/glue/mt_trace.c, perf builds: MT_TUNE + MT_TRACE; task 18) against the
 * fake OS: the ring, Control MT_CS_TRACE, the frame fields, and every hook on the paths an echo round takes (gate,
 * Control entry, PB completion, the core's DATA event, EWrite issue and completion, the tick). The clock is pbfake's
 * pbf_trace_us, which advances by pbf_trace_step per read; mt_trace_ew_done is called as the Mac's completion stub
 * would call it. */
#include "t.h"
#include "pbfake.h"
#include "mt_trace.h"
#include "mt_pb_entry.h"
#include "mt_tune_abi.h"
#include "mt_int.h"

#define TRAP_SYNC  0xA004u
#define TRAP_ASYNC (0xA004u | MT_DRV_TRAP_ASYNC)

static MtTrace T;

static void tr_setup(void)
{
    pbf_reset();
    memset(&T, 0, sizeof T);
    mt_trace_set(&T);
    pbf_trace_us = 1000;
    pbf_trace_step = 10;
}

/* the kept records, oldest first (straight from the ring) */
static int kept(MtTraceRec *out, int max)
{
    uint32_t n = T.total < MT_TRACE_RECS ? T.total : MT_TRACE_RECS, i;
    uint32_t first = T.total < MT_TRACE_RECS ? 0 : T.next;
    for (i = 0; i < n && (int)i < max; i++) out[i] = T.rec[(first + i) % MT_TRACE_RECS];
    return (int)n;
}

/* ---- the ring ---- */

static void test_ring_records_in_order_and_keeps_the_newest(void)
{
    uint32_t i;
    MtTraceRec r[4];
    tr_setup();
    mt_trace_ev(MT_TRACE_TICK, 1, 2, 3, 4);
    T_EQ(T.total, 1);
    T_EQ(T.rec[0].t, 1000);
    T_EQ(T.rec[0].ev, MT_TRACE_TICK);
    T_EQ(T.rec[0].a, 1);
    T_EQ(T.rec[0].b, 2);
    T_EQ(T.rec[0].c, 3);
    T_EQ(T.rec[0].d, 4);
    T_CHECK(pbf_trace_mask_seen > 0);                   /* the clock is read inside the mask */
    T_EQ(pbf_mask, 0);
    for (i = 1; i < MT_TRACE_RECS + 5; i++) mt_trace_ev(MT_TRACE_TICK, 0, 0, i, 0);
    T_EQ(T.total, MT_TRACE_RECS + 5);
    T_EQ(kept(r, 4), MT_TRACE_RECS);
    T_EQ(r[0].c, 5);                                    /* the oldest five were overwritten */
    T_EQ(r[1].c, 6);
    T_EQ(r[1].t - r[0].t, 10);
    T_EQ(pbf_bad, 0);
}

static void test_no_ring_or_frozen_records_nothing(void)
{
    tr_setup();
    mt_trace_set(NULL);
    mt_trace_ev(MT_TRACE_TICK, 0, 0, 0, 0);
    T_EQ(T.total, 0);
    T_EQ(pbf_trace_us, 1000);
    mt_trace_set(&T);
    T.frozen = 1;
    mt_trace_ev(MT_TRACE_TICK, 0, 0, 0, 0);
    mt_trace_frame(MT_TRACE_RX, 0, (const uint8_t *)"\0\0\0\0\0\0\0\0\0\0\0\0\x08\x06", 14);
    T_EQ(T.total, 0);
    T_EQ(pbf_trace_us, 1000);                           /* frozen: no clock read either */
}

/* ---- frame fields ---- */

static void test_frame_fields(void)
{
    uint8_t f[1600];
    uint8_t a;
    uint16_t b, n;
    uint32_t c, d;
    tr_setup();
    n = mk_tcp(f, T_PEER, 17007, T_ME, 1025, 0x11223344u, 7, TF_ACK | TF_PSH, 8192, NULL, 0,
               (const uint8_t *)"x", 1);
    mt_trace_frame_fields(f, n, &a, &b, &c, &d);
    T_EQ(a, TF_ACK | TF_PSH);
    T_EQ(b, 1);
    T_EQ(c, 0x11223344u);
    T_EQ(d, (17007u << 16) | 1025u);
    n = mk_tcp(f, T_ME, 1025, T_PEER, 17007, 9, 10, TF_ACK, 8192, NULL, 0, NULL, 0);
    mt_trace_frame_fields(f, n, &a, &b, &c, &d);
    T_EQ(a, TF_ACK);
    T_EQ(b, 0);
    T_EQ(c, 9);
    mt_trace_frame_fields(f, 30, &a, &b, &c, &d);     /* cut inside the TCP header: not TCP */
    T_EQ(a, MT_TRACE_A_NOTTCP);
    T_EQ(b, 30);
    T_EQ(c, 0x08000006u);
    {
        static const uint8_t mac[6] = { 2, 0, 0, 0, 0, 1 }, none[6] = { 0 };
        n = mk_arp(f, 1, mac, T_PEER, none, T_ME);
    }
    mt_trace_frame_fields(f, n, &a, &b, &c, &d);
    T_EQ(a, MT_TRACE_A_NOTTCP);
    T_EQ(b, n);
    T_EQ(c, 0x08060000u);
    T_EQ(d, 0);
    {
        static const uint8_t u[9] = { 0, 53, 0x04, 0x06, 0, 9, 0, 0, 'q' };   /* 53 -> 1030 */
        n = mk_ip(f, T_PEER, T_ME, 17, 1, 0, u, 9);
    }
    mt_trace_frame_fields(f, n, &a, &b, &c, &d);
    T_EQ(a, MT_TRACE_A_NOTTCP);
    T_EQ(c, 0x08000011u);
    T_EQ(d, (53u << 16) | 1030u);
    mt_trace_frame_fields(f, 5, &a, &b, &c, &d);      /* shorter than an Ethernet header */
    T_EQ(a, MT_TRACE_A_NOTTCP);
    T_EQ(c, 0);
    mt_trace_frame(MT_TRACE_EWRITE, 1, f, n);
    T_EQ(T.rec[0].a, MT_TRACE_A_NOTTCP | MT_TRACE_A_MARK);
}

/* ---- Control MT_CS_TRACE through the dispatcher ---- */

static int16_t trace_call(pbt *x, MtTraceBlk *blk, uint16_t op)
{
    TCPiopb *pb = pbt_pb(MT_CS_TRACE, 0);
    MacPtr32 a = pbf_a(blk);
    blk->op = op;
    memcpy((uint8_t *)pb + 28, &a, 4);
    return pbt_call(x, pb);
}

static void test_control_info_freeze_read_clear(void)
{
    pbt x;
    MtTraceBlk *blk;
    MtTraceRec *buf;
    int i;
    pbt_init(&x, 1);
    memset(&T, 0, sizeof T);
    mt_trace_set(&T);
    pbf_trace_us = 5000;
    pbf_trace_step = 10;
    T.lap_us = 225;
    blk = (MtTraceBlk *)pbf_alloc(sizeof *blk);
    buf = (MtTraceRec *)pbf_alloc(8 * sizeof *buf);
    for (i = 0; i < 20; i++) mt_trace_ev(MT_TRACE_TICK, 0, 0, (uint32_t)i, 0);
    blk->version = MT_TRACE_ABI_VERSION;
    T_EQ(trace_call(&x, blk, MT_TRACE_OP_INFO), 0);
    T_EQ(blk->got, 20);
    T_EQ(blk->total, 20);
    T_EQ(blk->cap, MT_TRACE_RECS);
    T_EQ(blk->lap_us, 225);
    T_EQ(blk->rec_bytes, 16);
    T_EQ(blk->flags & MT_TRACE_F_FROZEN, 0);
    T_CHECK(blk->now_us >= 5000 + 20 * 10);
    T_EQ(T.total, 20);                                  /* the trace call itself is not recorded */
    blk->buf = pbf_a(buf);
    blk->max = 8;
    blk->first = 0;
    T_EQ(trace_call(&x, blk, MT_TRACE_OP_READ), MT_TRACE_PARAMERR);   /* not frozen */
    T_EQ(trace_call(&x, blk, MT_TRACE_OP_FREEZE), 0);
    T_EQ(blk->flags & MT_TRACE_F_FROZEN, MT_TRACE_F_FROZEN);
    mt_trace_ev(MT_TRACE_TICK, 0, 0, 99, 0);
    T_EQ(T.total, 20);
    blk->first = 16;
    T_EQ(trace_call(&x, blk, MT_TRACE_OP_READ), 0);
    T_EQ(blk->got, 4);
    T_EQ(buf[0].c, 16);
    T_EQ(buf[3].c, 19);
    T_EQ(buf[3].ev, MT_TRACE_TICK);
    blk->first = 20;
    T_EQ(trace_call(&x, blk, MT_TRACE_OP_READ), 0);
    T_EQ(blk->got, 0);
    blk->buf = 0;
    blk->first = 0;
    T_EQ(trace_call(&x, blk, MT_TRACE_OP_READ), MT_TRACE_PARAMERR);
    blk->version = 2;
    T_EQ(trace_call(&x, blk, MT_TRACE_OP_INFO), MT_TRACE_PARAMERR);
    blk->version = MT_TRACE_ABI_VERSION;
    T_EQ(trace_call(&x, blk, 9), MT_TRACE_PARAMERR);
    T_EQ(trace_call(&x, blk, MT_TRACE_OP_CLEAR), 0);
    T_EQ(blk->got, 0);
    T_EQ(blk->total, 0);
    T_EQ(blk->flags & MT_TRACE_F_FROZEN, 0);
    mt_trace_ev(MT_TRACE_TICK, 0, 0, 7, 0);
    T_EQ(T.total, 1);
    T_EQ(pbf_bad, 0);
}

static void test_control_reads_a_wrapped_ring_oldest_first(void)
{
    pbt x;
    MtTraceBlk *blk;
    MtTraceRec *buf;
    uint32_t i;
    pbt_init(&x, 1);
    memset(&T, 0, sizeof T);
    mt_trace_set(&T);
    blk = (MtTraceBlk *)pbf_alloc(sizeof *blk);
    buf = (MtTraceRec *)pbf_alloc(4 * sizeof *buf);
    for (i = 0; i < MT_TRACE_RECS + 3; i++) mt_trace_ev(MT_TRACE_TICK, 0, 0, i, 0);
    blk->version = MT_TRACE_ABI_VERSION;
    T_EQ(trace_call(&x, blk, MT_TRACE_OP_FREEZE), 0);
    T_EQ(blk->got, MT_TRACE_RECS);
    T_EQ(blk->total, MT_TRACE_RECS + 3);
    blk->buf = pbf_a(buf);
    blk->max = 4;
    blk->first = 0;
    T_EQ(trace_call(&x, blk, MT_TRACE_OP_READ), 0);
    T_EQ(blk->got, 4);
    T_EQ(buf[0].c, 3);
    T_EQ(buf[3].c, 6);
    blk->first = MT_TRACE_RECS - 2;
    T_EQ(trace_call(&x, blk, MT_TRACE_OP_READ), 0);
    T_EQ(blk->got, 2);
    T_EQ(buf[1].c, MT_TRACE_RECS + 2);
    T_EQ(pbf_bad, 0);
}

/* ---- the gate: frames from the protocol handler, deferrals ---- */

static int g_rx_runs;
static pbt *g_rx_x;
static void rx_cb(void *ctx, const uint8_t *f, uint16_t len)
{
    (void)ctx;
    g_rx_runs++;
    if (g_rx_x) mt_pb_input(g_rx_x->d, f, len);
}
static void tick_cb(void *ctx) { (void)ctx; }

static void handler_frame(MtGate *g, const uint8_t *f, uint16_t n)   /* what mt_enet_ph.s does */
{
    MtRxSlot *s = mt_gate_slot(g);
    T_CHECK(s != NULL);
    if (!s) return;
    memcpy(s->buf, f, n);
    mt_gate_commit(g, n);
    mt_gate_poke(g);
}

static void test_gate_rx_deferral_and_run(void)
{
    MtGate g;
    uint8_t f[1600];
    uint16_t n;
    MtTraceRec r[8];
    tr_setup();
    g_rx_runs = 0;
    g_rx_x = NULL;
    mt_gate_init(&g, NULL, rx_cb, tick_cb, NULL);
    n = mk_tcp(f, T_PEER, 17007, T_ME, 1025, 500, 9, TF_ACK | TF_PSH, 8192, NULL, 0, (const uint8_t *)"e", 1);
    handler_frame(&g, f, n);                            /* core free: runs at once */
    T_EQ(g_rx_runs, 1);
    T_EQ(kept(r, 8), 1);
    T_EQ(r[0].ev, MT_TRACE_RX);
    T_EQ(r[0].a, TF_ACK | TF_PSH);
    T_EQ(r[0].b, 1);
    T_EQ(r[0].c, 500);
    mt_gate_enter(&g);                                  /* the application owns the core */
    handler_frame(&g, f, n);
    mt_gate_tick(&g);
    T_EQ(g_rx_runs, 1);
    T_EQ(kept(r, 8), 3);
    T_EQ(r[1].ev, MT_TRACE_RX);
    T_EQ(r[1].a, TF_ACK | TF_PSH | MT_TRACE_A_MARK);   /* deferred */
    T_EQ(r[2].ev, MT_TRACE_TDEFER);
    mt_gate_leave(&g);                                  /* the owner runs it */
    T_EQ(g_rx_runs, 2);
    T_EQ(kept(r, 8), 4);
    T_EQ(r[3].ev, MT_TRACE_RXRUN);
    T_EQ(r[3].c, 500);
    handler_frame(&g, f, n);                            /* the slot's mark does not stick */
    T_EQ(kept(r, 8), 5);
    T_EQ(r[4].ev, MT_TRACE_RX);
    T_EQ(r[4].a & MT_TRACE_A_MARK, 0);
    T_EQ(pbf_bad, 0);
}

/* ---- an echo round through the driver: entry, EWrite, gate, core, completion, read ---- */

static MtEnet E;
static MtEnetPump PU;
static MtGate G;
static MtEntry EN;

static int pump_tx(void *ctx, const uint8_t *frame, uint16_t len)
{
    (void)ctx;
    return mt_enet_tx_write(&E, &PU, frame, len);
}

static pbt *g_wire_x;
static void wire_hook(MtEnet *e, MtEnetTx *s)          /* issued frames also reach the fake link (pbt_last_tcp) */
{
    (void)e;
    if (g_wire_x && g_wire_x->fl.n < 64) {
        memcpy(g_wire_x->fl.f[g_wire_x->fl.n], s->frame, s->wds[0].len);
        g_wire_x->fl.len[g_wire_x->fl.n++] = s->wds[0].len;
    }
}

static void echo_setup(pbt *x)
{
    pbt_init(x, 1);
    memset(&T, 0, sizeof T);
    mt_trace_set(&T);
    pbf_trace_us = 1000;
    pbf_trace_step = 10;
    mt_enet_init(&E);
    memset(&PU, 0, sizeof PU);
    E.refnum = -48;
    E.opened = 1;
    x->d->s->link.ctx = NULL;
    x->d->s->link.tx = pump_tx;
    x->d->txpump = &PU.on;
    g_wire_x = x;
    pbf_enet_io_hook = wire_hook;
    g_rx_runs = 0;
    g_rx_x = x;
    mt_gate_init(&G, NULL, rx_cb, tick_cb, NULL);
    mt_entry_init(&EN, x->d, &G);
}

static MtEnetTx *in_flight(void)
{
    unsigned i;
    for (i = 0; i < MT_ENET_TXSLOTS; i++)
        if (E.tx[i].started && E.tx[i].pb.ioResult == 1) return &E.tx[i];
    return NULL;
}

static void ew_complete(void)                           /* the Ethernet driver's IODone */
{
    MtEnetTx *s = in_flight();
    T_CHECK(s != NULL);
    if (!s) return;
    T_EQ(s->pb.ioCompletion, mt_os_trace_ew_compl());
    s->pb.ioResult = 0;
    mt_trace_ew_done(&s->pb);
}

static TCPiopb *send1(StreamPtr s, const char *data)
{
    TCPiopb *pb = pbt_pb(TCPSend, s);
    wdsEntry *w = (wdsEntry *)pbf_alloc(2 * sizeof(wdsEntry));
    uint8_t *b = (uint8_t *)pbf_alloc(1);
    wdsEntry e[2];
    b[0] = (uint8_t)data[0];
    e[0].length = 1; e[0].ptr = pbf_a(b);
    e[1].length = 0; e[1].ptr = 0;
    memcpy(w, e, sizeof e);
    pb->csParam.send.wdsPtr = pbf_a(w);
    pb->csParam.send.pushFlag = 1;
    pb->ioCompletion = 0;                               /* a sync call: no routine */
    pb->ioResult = 1;
    return pb;
}

static TCPiopb *rcv1(StreamPtr s)
{
    TCPiopb *pb = pbt_pb(TCPRcv, s);
    uint8_t *b = (uint8_t *)pbf_alloc(16);
    pb->csParam.receive.rcvBuff = pbf_a(b);
    pb->csParam.receive.rcvBuffLen = 16;
    pb->ioCompletion = 0;
    pb->ioResult = 1;
    return pb;
}

static void echo_round(int pump)
{
    pbt x;
    pbt_conn c;
    StreamPtr s;
    TCPiopb *pb, *rb;
    uint8_t f[1600];
    uint16_t n;
    MtTraceRec r[24];
    int k, i;
    /* direct: the data EWrite is issued inside the TCPSend call; pumped: after it returned, by the pump's task */
    static const uint8_t want_direct[] = { MT_TRACE_PBIN, MT_TRACE_EWRITE, MT_TRACE_PBOUT, MT_TRACE_EWDONE,
                                           MT_TRACE_RX, MT_TRACE_PBDONE, MT_TRACE_DATA, MT_TRACE_EWRITE,
                                           MT_TRACE_EWDONE, MT_TRACE_PBIN, MT_TRACE_PBDONE, MT_TRACE_PBOUT };
    static const uint8_t want_pumped[] = { MT_TRACE_PBIN, MT_TRACE_PBOUT, MT_TRACE_EWRITE, MT_TRACE_EWDONE,
                                           MT_TRACE_RX, MT_TRACE_PBDONE, MT_TRACE_DATA, MT_TRACE_EWRITE,
                                           MT_TRACE_EWDONE, MT_TRACE_PBIN, MT_TRACE_PBDONE, MT_TRACE_PBOUT };
    const uint8_t *want = pump ? want_pumped : want_direct;
    const int ew = pump ? 2 : 1, out = pump ? 1 : 2;
    echo_setup(&x);
    s = pbt_est(&x, 0, 0, &c);
    T_CHECK(s != 0);
    PU.on = (uint8_t)pump;
    while (in_flight()) ew_complete();
    T.total = 0;
    T.next = 0;

    pb = send1(s, "A");                                 /* MoofBatch mode E: sync TCPSend of 1 byte, push */
    T_EQ(mt_entry_control(&EN, (MtAnyPb *)pb, TRAP_SYNC, 0), 0);
    T_EQ(pb->ioResult, inProgress);
    if (pump) mt_enet_pump_run(&E, &PU);                /* the pump's Time Manager task */
    ew_complete();
    n = mk_tcp(f, T_PEER, PBT_PEER_PORT, T_ME, c.lport, c.ps, c.ms + 1, TF_ACK | TF_PSH, 8192, NULL, 0,
               (const uint8_t *)"A", 1);
    handler_frame(&G, f, n);                            /* the echo, ACKing our byte */
    T_EQ(pb->ioResult, 0);
    if (pump) mt_enet_pump_run(&E, &PU);
    ew_complete();
    rb = rcv1(s);                                       /* sync TCPRcv: the byte is there */
    T_EQ(mt_entry_control(&EN, (MtAnyPb *)rb, TRAP_SYNC, 0), 0);
    T_EQ(rb->ioResult, 0);

    k = kept(r, 24);
    T_EQ(k, (int)sizeof want_direct);
    for (i = 0; i < k && i < (int)sizeof want_direct; i++) {
        if (r[i].ev != want[i]) fprintf(stderr, "record %d: ev %u, want %u\n", i, r[i].ev, want[i]);
        T_EQ(r[i].ev, want[i]);
        if (i) T_CHECK(r[i].t > r[i - 1].t);
    }
    if (k < (int)sizeof want_direct) return;
    T_EQ(r[0].b, TCPSend);
    T_EQ(r[0].a, 0);                                    /* sync, task time */
    T_EQ(r[0].c, pbf_a(pb));
    T_EQ(r[ew].a, TF_ACK | TF_PSH | (pump ? MT_TRACE_A_MARK : 0));
    T_EQ(r[ew].b, 1);
    T_EQ(r[ew].c, c.ms);
    T_EQ(r[ew].d, ((uint32_t)c.lport << 16) | PBT_PEER_PORT);
    T_EQ(r[out].b, TCPSend);
    T_EQ(r[out].d, 1);                                  /* result 0, ioResult 1 (in progress) */
    T_EQ(r[3].a, TF_ACK | TF_PSH);
    T_EQ(r[4].a, TF_ACK | TF_PSH);
    T_EQ(r[4].c, c.ps);
    T_EQ(r[5].b, TCPSend);
    T_EQ(r[5].a, 0);                                    /* no completion routine */
    T_EQ(r[5].c, pbf_a(pb));
    T_EQ(r[5].d, 0);
    T_EQ(r[6].c, c.ps);
    T_EQ(r[6].d, 1);
    T_EQ(r[7].a, TF_ACK | (pump ? MT_TRACE_A_MARK : 0));
    T_EQ(r[7].b, 0);
    T_EQ(r[9].b, TCPRcv);
    T_EQ(r[10].b, TCPRcv);
    T_EQ(r[11].b, TCPRcv);
    T_EQ(r[11].d, 0);
    T_EQ(pbf_bad, 0);
}

static void test_echo_round_direct(void) { echo_round(0); }
static void test_echo_round_pumped(void) { echo_round(1); }

static void test_trace_and_tune_calls_not_recorded(void)
{
    pbt x;
    TCPiopb *pb;
    uint16_t *w;
    MtTraceBlk *blk;
    MacPtr32 a;
    echo_setup(&x);
    blk = (MtTraceBlk *)pbf_alloc(sizeof *blk);
    blk->version = MT_TRACE_ABI_VERSION;
    blk->op = MT_TRACE_OP_INFO;
    pb = pbt_pb(MT_CS_TRACE, 0);
    a = pbf_a(blk);
    memcpy((uint8_t *)pb + 28, &a, 4);
    T_EQ(mt_entry_control(&EN, (MtAnyPb *)pb, TRAP_SYNC, 0), 0);
    w = (uint16_t *)pbf_alloc(MT_TUNE_BLOCK_BYTES);
    w[MT_TW_VERSION] = MT_TUNE_ABI_VERSION;
    w[MT_TW_OP] = MT_TUNE_OP_GET;
    pb = pbt_pb(MT_CS_TUNE, 0);
    a = pbf_a(w);
    memcpy((uint8_t *)pb + 28, &a, 4);
    T_EQ(mt_entry_control(&EN, (MtAnyPb *)pb, TRAP_SYNC, 0), 0);
    T_EQ(T.total, 0);
    pb = pbt_pb(TCPGlobalInfo, 0);                      /* any other call is */
    (void)mt_entry_control(&EN, (MtAnyPb *)pb, TRAP_ASYNC, 1);
    T_CHECK(T.total >= 2);
    T_EQ(T.rec[0].ev, MT_TRACE_PBIN);
    T_EQ(T.rec[0].a, MT_TRACE_A_ASYNC | MT_TRACE_A_IRQ);
    T_EQ(T.rec[0].b, TCPGlobalInfo);
}

static void test_tick_recorded(void)
{
    pbt x;
    echo_setup(&x);
    pbf_now = 4242;
    mt_pb_tick(x.d);
    T_EQ(T.total, 1);
    T_EQ(T.rec[0].ev, MT_TRACE_TICK);
    T_EQ(T.rec[0].c, 4242);
}

static void test_refused_call_completion_recorded(void)
{
    pbt x;
    TCPiopb *pb;
    echo_setup(&x);
    G.depth = 1;                                        /* the core is owned; a sync call at interrupt time */
    pb = pbt_pb(TCPStatus, 0);
    T_EQ(mt_entry_control(&EN, (MtAnyPb *)pb, TRAP_SYNC, 1), MT_DRV_REFUSED);
    G.depth = 0;
    T_EQ(T.total, 3);
    T_EQ(T.rec[1].ev, MT_TRACE_PBDONE);
    T_EQ((int32_t)T.rec[1].d, MT_DRV_REFUSED);
    T_EQ(T.rec[2].ev, MT_TRACE_PBOUT);
}

T_MAIN(test_ring_records_in_order_and_keeps_the_newest, test_no_ring_or_frozen_records_nothing, test_frame_fields,
       test_control_info_freeze_read_clear, test_control_reads_a_wrapped_ring_oldest_first,
       test_gate_rx_deferral_and_run, test_echo_round_direct, test_echo_round_pumped,
       test_trace_and_tune_calls_not_recorded, test_tick_recorded, test_refused_call_completion_recorded)
