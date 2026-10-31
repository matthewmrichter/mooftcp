/* test_enet_watchdog.c - the transmit slot watchdog (mac/glue/mt_enet_wd.c) against the fake OS.
 * "The driver" is the test: a started slot is a PB with ioResult 1 until the test completes it. */
#include "t.h"
#include "pbfake.h"
#include "mt_enet.h"

static MtEnet E;

static void setup(void)
{
    pbf_reset();
    memset(&E, 0, sizeof E);
    E.refnum = -48;
    E.opened = 1;
}

/* what mt_enet_write does after claiming a slot, minus the Toolbox */
static MtEnetTx *start(void)
{
    MtEnetTx *s = mt_enet_tx_claim(&E);
    if (!s) return NULL;
    memset(&s->pb, 0, sizeof s->pb);
    s->pb.ioResult = 1;
    s->started = 1;
    if (!E.tx_started++) E.wd_fresh = 1;
    return s;
}

static void watchdog_idle_costs_nothing(void)
{
    uint32_t t;
    setup();
    for (t = 0; t < 10000; t += 7) mt_enet_watchdog(&E, t);
    T_EQ(pbf_kill_calls, 0);
    T_EQ(E.tx_stuck, 0);
    T_EQ(mt_enet_wd_deadline(&E, 12345), 12345);          /* no cap while nothing is started */
    T_EQ(mt_enet_wd_deadline(&E, 0xFFFFFFFFu), 0xFFFFFFFFu);
    /* a finished write is reaped by the next scan, after which the watchdog idles again */
    {
        MtEnetTx *s = start();
        T_CHECK(s);
        T_EQ(mt_enet_wd_deadline(&E, 0xFFFFFFFFu), MT_ENET_WD_SCAN_MS);
        s->pb.ioResult = 0;
        mt_enet_watchdog(&E, 100);
        T_EQ(E.tx_started, 0);
        T_EQ(E.tx_err, 0);
    }
}

static void watchdog_ignores_slow_but_completing_write(void)
{
    uint32_t t;
    MtEnetTx *s;
    setup();
    s = start();
    for (t = 0; t < MT_ENET_TX_STUCK_MS - 1; t += 50) mt_enet_watchdog(&E, t);
    T_EQ(E.tx_stuck, 0);
    s->pb.ioResult = 0;                                    /* completes just in time */
    mt_enet_watchdog(&E, MT_ENET_TX_STUCK_MS + 500);
    T_EQ(E.tx_stuck, 0);
    T_EQ(pbf_kill_calls, 0);
    T_EQ(E.tx_started, 0);
    T_EQ(E.reattach_req, 0);
}

static void watchdog_kills_after_timeout(void)
{
    uint32_t t;
    MtEnetTx *s;
    setup();
    s = start();
    for (t = 0; t <= MT_ENET_TX_STUCK_MS + 200; t += 50) mt_enet_watchdog(&E, t);
    T_EQ(E.tx_stuck, 1);
    T_EQ(E.tx_killed, 1);
    T_EQ(pbf_kill_calls, 1);
    T_EQ(pbf_kill_ref, -48);
    T_CHECK(pbf_kill_pb == (void *)&s->pb);
    for (t = 2300; t < 2300 + MT_ENET_TX_STUCK_MS - 300; t += 50) mt_enet_watchdog(&E, t);
    T_EQ(pbf_kill_calls, 1);                               /* once */
    T_EQ(E.reattach_req, 0);
    /* the kill works: the driver completes it with abortErr; counted as an error, slot is free */
    s->pb.ioResult = -27;
    mt_enet_watchdog(&E, 4000);
    T_EQ(E.tx_err, 1);
    T_EQ(E.tx_started, 0);
    T_EQ(E.reattach_req, 0);
    T_EQ(mt_enet_tx_busy(&E), 0);
}

static void watchdog_reattach_after_second_timeout(void)
{
    uint32_t t;
    MtEnetTx *s;
    setup();
    s = start();
    for (t = 0; t <= 2 * MT_ENET_TX_STUCK_MS + 400; t += 50) mt_enet_watchdog(&E, t);
    T_EQ(E.tx_stuck, 1);
    T_EQ(pbf_kill_calls, 1);
    T_EQ(E.reattach_req, 1);
    T_EQ(mt_enet_tx_quarantined(&E), 1);
    T_EQ(E.tx_started, 0);                                 /* no longer watched: no more 100 ms ticks */
    T_EQ(mt_enet_wd_deadline(&E, 0xFFFFFFFFu), 0xFFFFFFFFu);
    for (t = 5000; t < 60000; t += 100) mt_enet_watchdog(&E, t);
    T_EQ(pbf_kill_calls, 1);                               /* never killed twice, no new requests */
    /* late completion by the driver after the reattach: the slot becomes usable again */
    E.reattach_req = 0;
    s->pb.ioResult = 0;
    T_CHECK(mt_enet_tx_claim(&E) != NULL);
    T_EQ(mt_enet_tx_quarantined(&E), 0);
}

static void watchdog_never_reuses_in_flight_slot(void)
{
    unsigned i;
    MtEnetTx *held[MT_ENET_TXSLOTS], *x;
    uint32_t t;
    setup();
    for (i = 0; i < MT_ENET_TXSLOTS; i++) {
        held[i] = start();
        T_CHECK(held[i]);
        if (i) T_CHECK(held[i] != held[i - 1]);
    }
    T_CHECK(start() == NULL);                              /* all twelve in flight */
    for (t = 0; t < 3 * MT_ENET_TX_STUCK_MS; t += 100) mt_enet_watchdog(&E, t);
    T_EQ(E.tx_stuck, MT_ENET_TXSLOTS);
    T_EQ(mt_enet_tx_quarantined(&E), (int)MT_ENET_TXSLOTS);
    T_CHECK(mt_enet_tx_claim(&E) == NULL);                 /* quarantined: link dead, never reused */
    for (i = 0; i < MT_ENET_TXSLOTS; i++) T_EQ(held[i]->pb.ioResult, 1);
    /* one comes back: exactly that slot, nothing else */
    held[5]->pb.ioResult = 0;
    x = mt_enet_tx_claim(&E);
    T_CHECK(x == held[5]);
    {
        unsigned n = 0;
        for (i = 0; i < MT_ENET_TXSLOTS; i++) if (E.tx[i].pb.ioResult > 0) n++;
        T_EQ(n, MT_ENET_TXSLOTS - 1);
    }
}

static void watchdog_wrap_safe_time(void)
{
    uint32_t t = 0xFFFFFFFFu - 1500u, end = t + 2 * MT_ENET_TX_STUCK_MS + 400u;   /* crosses 2^32 */
    MtEnetTx *s;
    setup();
    E.wd_next = 0;                                          /* a stale value must not block the scan */
    s = start();
    (void)s;
    for (; t != end; t += 50u) mt_enet_watchdog(&E, t);
    T_EQ(E.tx_stuck, 1);
    T_EQ(pbf_kill_calls, 1);
    T_EQ(E.reattach_req, 1);
}

static void watchdog_scan_period(void)
{
    MtEnetTx *s;
    setup();
    s = start();
    mt_enet_watchdog(&E, 1000);                             /* first look: fresh */
    T_EQ(E.wd_fresh, 0);
    mt_enet_watchdog(&E, 1000 + MT_ENET_WD_SCAN_MS - 1);    /* too early: no scan */
    T_EQ(s->wd_t0, 1000);
    s->pb.ioResult = 0;
    mt_enet_watchdog(&E, 1000 + MT_ENET_WD_SCAN_MS - 1);
    T_EQ(E.tx_started, 1);                                  /* still unscanned */
    mt_enet_watchdog(&E, 1000 + MT_ENET_WD_SCAN_MS);
    T_EQ(E.tx_started, 0);
}

T_MAIN(watchdog_idle_costs_nothing, watchdog_ignores_slow_but_completing_write,
       watchdog_kills_after_timeout, watchdog_reattach_after_second_timeout,
       watchdog_never_reuses_in_flight_slot, watchdog_wrap_safe_time, watchdog_scan_period)
