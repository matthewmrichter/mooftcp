/* test_enet_pump.c - the transmit pump (mac/glue/mt_enet_pump.c, perf builds: MT_TUNE lever txpump) against the
 * fake OS. "The driver" is pbfake's mt_enet_io_write: every EWrite the glue issues is counted there and handed to
 * pbf_enet_io_hook; the test completes a write by storing its ioResult. "The Time Manager task" is the test
 * calling mt_enet_pump_run; mt_enet_pump_prime only counts. */
#include "t.h"
#include "pbfake.h"
#include "mt_enet.h"
#include "mt_int.h"
#include "mt_tune_abi.h"

static MtEnet E;
static MtEnetPump PU;

/* the EWrites as the driver saw them, in issue order: first payload byte after the Ethernet header */
static uint8_t g_wire[64];
static int g_nwire;
static pbt *g_x;                        /* set: issued frames also go to its fake link (pbt_last_tcp sees them) */

static void wire_hook(MtEnet *e, MtEnetTx *s)
{
    (void)e;
    if (g_nwire < (int)sizeof g_wire) g_wire[g_nwire] = s->frame[14];
    g_nwire++;
    T_EQ(s->pb.csCode, 249);                             /* EWrite, on the PB the glue prepared */
    T_EQ(s->pb.ioRefNum, -48);
    T_EQ(s->wds[0].ptr, (uint32_t)(uintptr_t)s->frame);
    if (g_x && g_x->fl.n < 64) {
        memcpy(g_x->fl.f[g_x->fl.n], s->frame, s->wds[0].len);
        g_x->fl.len[g_x->fl.n++] = s->wds[0].len;
    }
}

static void setup(void)
{
    pbf_reset();
    mt_enet_init(&E);
    memset(&PU, 0, sizeof PU);
    E.refnum = -48;
    E.opened = 1;
    g_nwire = 0;
    g_x = NULL;
    pbf_enet_io_hook = wire_hook;
}

static int wr(uint8_t tag, uint16_t len)
{
    uint8_t f[1514];
    memset(f, 0, sizeof f);
    f[12] = 0x08;
    f[14] = tag;
    return mt_enet_tx_write(&E, &PU, f, len);
}

static void complete_all(int16_t res)
{
    unsigned i;
    for (i = 0; i < MT_ENET_TXSLOTS; i++)
        if (E.tx[i].started && E.tx[i].pb.ioResult == 1) E.tx[i].pb.ioResult = res;
}

/* Lever off, nothing queued: every frame is issued inside the call, in order, as mt_enet_write does; no prime. */
static void test_off_issues_at_once_in_order(void)
{
    setup();
    T_EQ(wr(1, 60), 0);
    T_EQ(g_nwire, 1);
    T_EQ(wr(2, 54), 0);                                  /* a pure ACK: padded to 60 */
    T_EQ(wr(3, 1514), 0);
    T_EQ(g_nwire, 3);
    T_EQ(g_wire[0], 1); T_EQ(g_wire[1], 2); T_EQ(g_wire[2], 3);
    T_EQ(pbf_pump_primes, 0);
    T_EQ(pbf_enet_write_calls, 3);                       /* off is mt_enet_write itself (release path) */
    T_EQ(E.tx_ok, 3); T_EQ(E.tx_pad, 1); T_EQ(E.tx_started, 3);
    T_EQ(mt_enet_tx_busy(&E), 3);
    T_EQ(PU.head, PU.tail);
}

/* Lever on: link.tx returns 0 at once and issues nothing; the first frame primes the task once; the task issues
 * every queued frame in order and is not primed again by its own run. */
static void test_on_queues_until_task_runs(void)
{
    setup();
    PU.on = 1;
    T_EQ(wr(1, 60), 0);
    T_EQ(g_nwire, 0);
    T_EQ(pbf_pump_primes, 1);
    T_EQ(wr(2, 60), 0);
    T_EQ(wr(3, 60), 0);
    T_EQ(g_nwire, 0);
    T_EQ(pbf_pump_primes, 1);                            /* primed once for the batch */
    T_EQ(E.tx_started, 3);                               /* counted when queued: the watchdog times them */
    T_EQ(mt_enet_tx_busy(&E), 3);                        /* queued slots are not free */
    mt_enet_pump_run(&E, &PU);
    T_EQ(g_nwire, 3);
    T_EQ(g_wire[0], 1); T_EQ(g_wire[1], 2); T_EQ(g_wire[2], 3);
    T_EQ(PU.issued, 3); T_EQ(PU.runs, 1);
    T_EQ(PU.armed, 0);
    T_EQ(pbf_pump_primes, 1);
    mt_enet_pump_run(&E, &PU);                           /* an empty run issues nothing */
    T_EQ(g_nwire, 3);
    complete_all(0);                                     /* the driver completes the three */
    mt_enet_watchdog(&E, 0);                             /* the owner reaps them */
    T_EQ(E.tx_started, 0);
    T_EQ(E.tx_err, 0);
    T_EQ(wr(4, 60), 0);                                  /* the next frame primes again */
    T_EQ(pbf_pump_primes, 2);
    T_EQ(E.tx_started, 1);
}

/* No free slot: busy (1), counted in tx_drop_busy; the core stalls and retries. A slot is free again only after
 * the task issued it and the driver completed it. */
static void test_ring_full_returns_busy(void)
{
    unsigned i;
    setup();
    PU.on = 1;
    for (i = 0; i < MT_ENET_TXSLOTS; i++) T_EQ(wr((uint8_t)i, 60), 0);
    T_EQ(wr(99, 60), 1);
    T_EQ(E.tx_drop_busy, 1);
    mt_enet_pump_run(&E, &PU);
    T_EQ(g_nwire, (int)MT_ENET_TXSLOTS);
    T_EQ(wr(99, 60), 1);                                 /* issued but still in flight */
    E.tx[0].pb.ioResult = 0;                             /* the driver completes one */
    T_EQ(wr(100, 60), 0);
    T_EQ(E.tx_drop_busy, 2);
    T_EQ(PU.max_q, MT_ENET_TXSLOTS);
}

/* Switching off while frames are queued never lets a later frame overtake them. */
static void test_off_with_queue_keeps_order(void)
{
    setup();
    PU.on = 1;
    T_EQ(wr(1, 60), 0);
    T_EQ(wr(2, 60), 0);
    PU.on = 0;
    T_EQ(wr(3, 60), 0);
    T_EQ(g_nwire, 0);                                    /* queued behind, not written directly */
    mt_enet_pump_run(&E, &PU);
    T_EQ(g_nwire, 3);
    T_EQ(g_wire[0], 1); T_EQ(g_wire[1], 2); T_EQ(g_wire[2], 3);
    T_EQ(wr(4, 60), 0);                                  /* queue empty again: direct */
    T_EQ(g_nwire, 4);
}

/* The driver delivers a frame inside the task's EWrite and the core answers it at once (DaynaPORT): the answer
 * joins the FIFO behind, the running task issues it in the same run and nothing primes a second run. */
static int g_nest_once;
static void nest_hook(MtEnet *e, MtEnetTx *s)
{
    wire_hook(e, s);
    if (g_nest_once) { g_nest_once = 0; T_EQ(wr(9, 54), 0); T_EQ(g_nwire, 1); }
}
static void test_nested_write_drained_by_same_run(void)
{
    setup();
    pbf_enet_io_hook = nest_hook;
    g_nest_once = 1;
    PU.on = 1;
    T_EQ(wr(1, 60), 0);
    T_EQ(wr(2, 60), 0);
    mt_enet_pump_run(&E, &PU);
    T_EQ(g_nwire, 3);
    T_EQ(g_wire[0], 1); T_EQ(g_wire[1], 2); T_EQ(g_wire[2], 9);
    T_EQ(pbf_pump_primes, 1);
    T_EQ(PU.nested, 1);
    T_EQ(PU.armed, 0); T_EQ(PU.running, 0);
}

/* A frame queued by an interrupt after the task's last look (running already 0) primes a new run itself. */
static void test_write_after_run_primes_again(void)
{
    setup();
    PU.on = 1;
    T_EQ(wr(1, 60), 0);
    mt_enet_pump_run(&E, &PU);
    T_EQ(wr(2, 60), 0);
    T_EQ(pbf_pump_primes, 2);
    T_CHECK(PU.armed);
}

/* An interrupt queues a frame after the task's last look but before it clears `running`: the frame does not prime
 * (the task is still running), so the task's re-check must issue it before it returns. */
static void window_hook(MtEnetPump *p)
{
    p->test_window = NULL;
    T_EQ(wr(7, 60), 0);
}
static void test_frame_in_the_last_window_not_stranded(void)
{
    setup();
    PU.on = 1;
    T_EQ(wr(1, 60), 0);
    PU.test_window = window_hook;
    mt_enet_pump_run(&E, &PU);
    T_EQ(g_nwire, 2);
    T_EQ(g_wire[1], 7);
    T_EQ(PU.head, PU.tail);
    T_EQ(pbf_pump_primes, 1);
}

/* An EWrite the driver refuses at once: the task only stores the error in the slot; the owner's next reap counts
 * it (tx_err, last_err) and frees the slot. A closed link fails the queued slots with -28 the same way. */
static void test_refused_and_closed_counted_by_owner(void)
{
    setup();
    PU.on = 1;
    T_EQ(wr(1, 60), 0);
    pbf_enet_io_result = -17;
    mt_enet_pump_run(&E, &PU);
    T_EQ(g_nwire, 1);
    T_EQ(E.tx_err, 0);                                   /* not written from the task */
    mt_enet_watchdog(&E, 0);
    T_EQ(E.tx_err, 1); T_EQ(E.last_err, -17); T_EQ(E.tx_started, 0);
    pbf_enet_io_result = 0;
    T_EQ(wr(2, 60), 0);
    E.opened = 0;                                        /* reattach / failed open between queue and task */
    mt_enet_pump_run(&E, &PU);
    T_EQ(g_nwire, 1);                                    /* not issued */
    mt_enet_watchdog(&E, 0);
    T_EQ(E.tx_err, 2); T_EQ(E.last_err, -28); T_EQ(E.tx_started, 0);
    T_EQ(mt_enet_tx_busy(&E), 0);
}

/* Watchdog KillIO / reattach (task time, gate owned) holds the pump: a task run issues nothing and clears armed;
 * the release primes again and the queued frames leave in order. Pumped frames never use mt_enet_write. */
static void test_hold_blocks_task_until_released(void)
{
    setup();
    PU.on = 1;
    T_EQ(wr(1, 60), 0);
    T_EQ(wr(2, 60), 0);
    mt_enet_pump_hold(&PU, 1);
    mt_enet_pump_run(&E, &PU);                           /* the task fires during the synchronous calls */
    T_EQ(g_nwire, 0);
    T_EQ(PU.armed, 0);
    T_EQ(PU.issued, 0);
    T_EQ(pbf_pump_primes, 1);
    mt_enet_pump_hold(&PU, 0);
    T_EQ(pbf_pump_primes, 2);                            /* re-primed for the frames still queued */
    T_CHECK(PU.armed);
    mt_enet_pump_run(&E, &PU);
    T_EQ(g_nwire, 2);
    T_EQ(g_wire[0], 1); T_EQ(g_wire[1], 2);
    mt_enet_pump_hold(&PU, 1);                           /* nothing queued: a release primes nothing */
    mt_enet_pump_hold(&PU, 0);
    T_EQ(pbf_pump_primes, 2);
    T_EQ(pbf_enet_write_calls, 0);
}

/* Drop paths before a slot is taken are today's: closed, bad length. */
static void test_closed_and_bad_length_dropped(void)
{
    setup();
    PU.on = 1;
    E.opened = 0;
    T_EQ(wr(1, 60), 1); T_EQ(E.tx_drop_closed, 1);
    E.opened = 1;
    T_EQ(wr(1, 13), 1); T_EQ(E.tx_drop_len, 1);
    T_EQ(wr(1, 1515), 1); T_EQ(E.tx_drop_len, 2);
    T_EQ(pbf_pump_primes, 0);
    T_EQ(mt_enet_tx_busy(&E), 0);
}

/* ---- through the driver: Control 0x4D55 and a write from inside receive delivery ---- */

static int pump_tx(void *ctx, const uint8_t *f, uint16_t len) { (void)ctx; return mt_enet_tx_write(&E, &PU, f, len); }

static int16_t tune_call(pbt *x, uint16_t *blk, uint16_t ver, uint16_t op)
{
    TCPiopb *pb = pbt_pb(MT_CS_TUNE, 0);
    uint16_t *b = (uint16_t *)pbf_alloc(MT_TUNE_BLOCK_BYTES);
    MacPtr32 a = pbf_a(b);
    int16_t r;
    memcpy(b, blk, MT_TUNE_BLOCK_BYTES);
    b[MT_TW_VERSION] = ver;
    b[MT_TW_OP] = op;
    memcpy((uint8_t *)pb + 28, &a, 4);
    r = pbt_call(x, pb);
    memcpy(blk, b, MT_TUNE_BLOCK_BYTES);
    return r;
}

static void drv_setup(pbt *x)
{
    pbt_init(x, 1);                                     /* resets the fake */
    mt_enet_init(&E);
    memset(&PU, 0, sizeof PU);
    E.refnum = -48;
    E.opened = 1;
    g_nwire = 0;
    g_x = x;
    pbf_enet_io_hook = wire_hook;
    x->d->s->link.ctx = NULL;                           /* the driver's link.tx in perf builds */
    x->d->s->link.tx = pump_tx;
    x->d->txpump = &PU.on;
}

static void test_control_txpump_v2_and_v1(void)
{
    pbt x;
    uint16_t w[MT_TW_N];
    drv_setup(&x);
    memset(w, 0, sizeof w);
    T_EQ(tune_call(&x, w, MT_TUNE_ABI_VERSION, MT_TUNE_OP_GET), 0);
    T_EQ(w[MT_TW_TXPUMP], 0);
    T_EQ(w[MT_TW_DELACK], MT_TCP_DELACK_MS);
    w[MT_TW_TXPUMP] = 1;
    T_EQ(tune_call(&x, w, MT_TUNE_ABI_VERSION, MT_TUNE_OP_SET), 0);
    T_EQ(PU.on, 1);
    memset(w, 0, sizeof w);
    T_EQ(tune_call(&x, w, MT_TUNE_ABI_VERSION, MT_TUNE_OP_GET), 0);
    T_EQ(w[MT_TW_TXPUMP], 1);
    T_EQ(tune_call(&x, w, MT_TUNE_ABI_V1, MT_TUNE_OP_GET), 0);   /* a v1 block: the word reads 0 */
    T_EQ(w[MT_TW_TXPUMP], 0);
    w[MT_TW_TXPUMP] = 2;                                         /* out of range: refused, nothing changes */
    w[MT_TW_DELACK] = 20;
    T_EQ(tune_call(&x, w, MT_TUNE_ABI_VERSION, MT_TUNE_OP_SET), MT_TUNE_PARAMERR);
    T_EQ(PU.on, 1);
    T_EQ(x.d->s->tune.delack_ms, MT_TCP_DELACK_MS);
    w[MT_TW_TXPUMP] = 7;                                         /* a v1 set: the word is ignored, txpump 0 */
    T_EQ(tune_call(&x, w, MT_TUNE_ABI_V1, MT_TUNE_OP_SET), 0);
    T_EQ(PU.on, 0);
    T_EQ(x.d->s->tune.delack_ms, 20);
    T_EQ(tune_call(&x, w, 3, MT_TUNE_OP_GET), MT_TUNE_PARAMERR);  /* unknown version */
    x.d->txpump = NULL;                                          /* a driver without a pump: txpump 1 refused */
    w[MT_TW_TXPUMP] = 1;
    T_EQ(tune_call(&x, w, MT_TUNE_ABI_VERSION, MT_TUNE_OP_SET), MT_TUNE_PARAMERR);
    w[MT_TW_TXPUMP] = 0;
    T_EQ(tune_call(&x, w, MT_TUNE_ABI_VERSION, MT_TUNE_OP_SET), 0);
    T_EQ(pbf_bad, 0);
}

/* The echo case: a 1-byte PSH segment arrives (receive delivery: mt_pb_input) and the core ACKs it at once. With
 * the pump on the ACK is not written from inside the delivery; it leaves when the pump's task runs. Off: it is
 * written inside the delivery, as today. */
static void test_ack_from_receive_delivery_waits_for_task(void)
{
    pbt x;
    pbt_conn c;
    uint16_t w[MT_TW_N];
    tcpv v;
    StreamPtr s;
    int k;
    for (k = 0; k < 2; k++) {
        drv_setup(&x);
        s = pbt_est(&x, 0, 0, &c);                           /* lever off: SYN written at once */
        T_CHECK(s != 0);
        memset(w, 0, sizeof w);
        T_EQ(tune_call(&x, w, MT_TUNE_ABI_VERSION, MT_TUNE_OP_GET), 0);
        w[MT_TW_TXPUMP] = (uint16_t)(k == 0);
        T_EQ(tune_call(&x, w, MT_TUNE_ABI_VERSION, MT_TUNE_OP_SET), 0);
        fake_clear(&x.fl);
        g_nwire = 0;
        pbt_peer(&x, c.lport, c.ps, c.ms, TF_ACK | TF_PSH, 8192, "e", 1);
        if (k == 0) {
            T_EQ(g_nwire, 0);                                /* nothing written inside the delivery */
            T_EQ(pbf_pump_primes, 1);
            mt_enet_pump_run(&E, &PU);                       /* the pump's Time Manager task */
        }
        T_EQ(g_nwire, 1);
        T_CHECK(pbt_last_tcp(&x, &v));
        T_EQ(v.flags, TF_ACK);
        T_EQ(v.ack, c.ps + 1);
    }
    T_EQ(pbf_bad, 0);
}

T_MAIN(test_off_issues_at_once_in_order, test_on_queues_until_task_runs, test_ring_full_returns_busy,
       test_off_with_queue_keeps_order, test_nested_write_drained_by_same_run, test_write_after_run_primes_again,
       test_frame_in_the_last_window_not_stranded,
       test_refused_and_closed_counted_by_owner, test_hold_blocks_task_until_released,
       test_closed_and_bad_length_dropped, test_control_txpump_v2_and_v1,
       test_ack_from_receive_delivery_waits_for_task)
