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

static MtEnetTx *g_kill_target;
static void kill_effect(void) { g_kill_target->pb.ioResult = -27; }   /* the driver aborts it */

static void watchdog_kills_after_timeout(void)
{
    uint32_t t;
    MtEnetTx *s;
    setup();
    s = start();
    for (t = 0; t <= MT_ENET_TX_STUCK_MS + 200; t += 50) mt_enet_watchdog(&E, t);
    T_EQ(E.tx_stuck, 1);
    T_EQ(E.kill_req, 1);
    T_EQ(pbf_kill_calls, 0);                               /* the watchdog itself never calls the driver */
    T_EQ(E.tx_killed, 0);
    /* task time: the kill is made, once, and counted apart from tx_stuck */
    pbf_kill_result = -50;                                 /* KillIO's own OSErr is kept */
    g_kill_target = s;
    pbf_kill_hook = kill_effect;
    mt_enet_service(&E, 2300);
    T_EQ(pbf_kill_calls, 1);
    T_EQ(E.tx_killed, 1);
    T_EQ(E.kill_req, 0);
    T_EQ(E.last_err, -50);
    T_EQ(pbf_kill_ref, -48);
    mt_enet_service(&E, 2400);
    T_EQ(pbf_kill_calls, 1);                               /* once */
    T_EQ(E.reattach_req, 0);
    mt_enet_watchdog(&E, 4000);                            /* the kill worked: slot is reaped */
    T_EQ(E.tx_err, 1);
    T_EQ(E.tx_started, 0);
    T_EQ(E.reattach_req, 0);
    T_EQ(mt_enet_tx_busy(&E), 0);
}

static void watchdog_kill_not_repeated_when_service_is_late(void)
{
    uint32_t t;
    setup();
    (void)start();
    (void)start();
    for (t = 0; t <= MT_ENET_TX_STUCK_MS + 200; t += 50) mt_enet_watchdog(&E, t);
    T_EQ(E.tx_stuck, 2);                                   /* two slots, one request */
    mt_enet_service(&E, 2300);
    T_EQ(pbf_kill_calls, 1);
    T_EQ(E.tx_killed, 1);
}

static void watchdog_reattach_after_second_timeout(void)
{
    uint32_t t;
    MtEnetTx *s;
    setup();
    s = start();
    for (t = 0; t <= 2 * MT_ENET_TX_STUCK_MS + 400; t += 50) mt_enet_watchdog(&E, t);
    T_EQ(E.tx_stuck, 1);
    T_EQ(pbf_kill_calls, 0);
    T_EQ(E.reattach_req, 1);
    T_EQ(mt_enet_tx_quarantined(&E), 1);
    T_EQ(E.tx_started, 0);                                 /* no longer watched: no more 100 ms ticks */
    T_EQ(mt_enet_wd_deadline(&E, 0xFFFFFFFFu), 0xFFFFFFFFu);
    T_EQ(pbf_reattach_calls, 0);                           /* nothing at interrupt time */
    mt_enet_service(&E, 4500);                             /* first task-time entry */
    T_EQ(pbf_kill_calls, 1);
    T_EQ(pbf_reattach_calls, 1);
    T_EQ(E.reattach, 1);
    T_EQ(E.reattach_req, 0);
    T_EQ(E.opened, 1);
    for (t = 5000; t < 60000; t += 100) { mt_enet_watchdog(&E, t); mt_enet_service(&E, t); }
    T_EQ(pbf_kill_calls, 1);
    T_EQ(pbf_reattach_calls, 1);                           /* no new requests */
    /* late completion by the driver after the reattach: only now the slot becomes usable again */
    T_EQ(s->pb.ioResult, 1);
    s->pb.ioResult = 0;
    T_CHECK(mt_enet_tx_claim(&E) != NULL);
    T_EQ(mt_enet_tx_quarantined(&E), 0);
}

static void watchdog_reattach_backoff_and_abandon(void)
{
    uint32_t t;
    setup();
    (void)start();
    for (t = 0; t <= 2 * MT_ENET_TX_STUCK_MS + 400; t += 50) mt_enet_watchdog(&E, t);
    pbf_reattach_result = -94;
    mt_enet_service(&E, 5000);                             /* attempt 1 fails */
    T_EQ(pbf_reattach_calls, 1);
    T_EQ(E.reattach_fail, 1);
    T_EQ(E.opened, 0);
    mt_enet_service(&E, 5999);                             /* backoff 1 s: too early */
    T_EQ(pbf_reattach_calls, 1);
    mt_enet_service(&E, 6000);                             /* attempt 2 */
    T_EQ(pbf_reattach_calls, 2);
    mt_enet_service(&E, 7999);                             /* backoff 2 s */
    T_EQ(pbf_reattach_calls, 2);
    mt_enet_service(&E, 8000);                             /* attempt 3 */
    T_EQ(pbf_reattach_calls, 3);
    mt_enet_service(&E, 11999);                            /* backoff 4 s */
    T_EQ(pbf_reattach_calls, 3);
    mt_enet_service(&E, 12000);                            /* attempt 4: the last */
    T_EQ(pbf_reattach_calls, 4);
    T_EQ(E.reattach_req, 0);
    T_EQ(E.reattach_abandoned, 1);
    T_EQ(E.reattach_fail, 4);
    mt_enet_service(&E, 99999);
    T_EQ(pbf_reattach_calls, 4);                           /* abandoned */
    T_EQ(E.last_err, -94);
}

static void watchdog_reattach_skipped_when_our_pb_heads_the_queue(void)
{
    MtEnetTx *s;
    uint32_t t;
    setup();
    s = start();
    for (t = 0; t <= 2 * MT_ENET_TX_STUCK_MS + 400; t += 50) mt_enet_watchdog(&E, t);
    pbf_dev_head = &s->pb;                                 /* the stuck PB is the active request */
    mt_enet_service(&E, 4500);
    T_EQ(pbf_reattach_calls, 0);                           /* a sync call would wait forever */
    T_EQ(E.reattach_blocked, 1);
    T_EQ(E.reattach_fail, 1);
    pbf_dev_head = NULL;                                   /* the queue moved on */
    mt_enet_service(&E, 5500);
    T_EQ(pbf_reattach_calls, 1);
    T_EQ(E.reattach, 1);
    pbf_dev_head = (const void *)&E;                       /* somebody else's PB does not block */
    T_EQ(mt_os_enet_blocked(-48, &E.tx[0], &E.tx[MT_ENET_TXSLOTS]), 0);
}

static void watchdog_no_reattach_request_when_link_is_closed(void)
{
    uint32_t t;
    setup();
    E.opened = 0;
    (void)start();
    for (t = 0; t <= 2 * MT_ENET_TX_STUCK_MS + 400; t += 50) mt_enet_watchdog(&E, t);
    T_EQ(E.reattach_req, 0);
    T_EQ(E.tx_stuck, 1);
}

static void watchdog_quarantine_keeps_started_count_exact(void)
{
    MtEnetTx *a, *b;
    uint32_t t;
    setup();
    a = start();
    for (t = 0; t < 2 * MT_ENET_TX_STUCK_MS + 200; t += 50) mt_enet_watchdog(&E, t);
    T_EQ(E.tx_started, 0);                                 /* a is quarantined */
    b = start();
    T_CHECK(b != a);
    T_EQ(E.tx_started, 1);
    a->pb.ioResult = 0;                                    /* a completes late, the scan reaps it */
    mt_enet_watchdog(&E, t);
    T_EQ(E.tx_started, 1);                                 /* b is still counted (not 0) */
    T_EQ(b->started, 1);
}

static void open_keeps_in_flight_slots(void)
{
    MtEnetTx *s, *again;
    unsigned i;
    setup();
    s = start();
    E.tx_ok = 7;
    mt_enet_link_reset(&E);                                /* what a re-open does first */
    T_EQ(E.tx_started, 1);
    T_EQ(s->started, 1);
    T_EQ(s->pb.ioResult, 1);                               /* the driver's PB was not zeroed */
    T_EQ(E.tx_ok, 7);
    T_EQ(E.opened, 0);
    for (i = 0; i < MT_ENET_TXSLOTS - 1; i++) {
        again = mt_enet_tx_claim(&E);
        T_CHECK(again && again != s);
        again->pb.ioResult = 1;
        again->started = 1;
    }
    T_CHECK(mt_enet_tx_claim(&E) == NULL);                 /* s is never handed out */
    mt_enet_init(&E);
    T_EQ(E.tx[0].pb.ioResult, 0);
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
    T_EQ(E.kill_req, 1);
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
       watchdog_kills_after_timeout, watchdog_kill_not_repeated_when_service_is_late,
       watchdog_reattach_after_second_timeout, watchdog_reattach_backoff_and_abandon,
       watchdog_reattach_skipped_when_our_pb_heads_the_queue,
       watchdog_no_reattach_request_when_link_is_closed, watchdog_quarantine_keeps_started_count_exact,
       open_keeps_in_flight_slots, watchdog_never_reuses_in_flight_slot, watchdog_wrap_safe_time,
       watchdog_scan_period)
