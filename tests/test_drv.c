/* test_drv.c - Task 10: the driver entry's pure decisions (mac/glue/mt_drv_logic.h), the prefs
 * parser (mac/glue/mt_cfg.h), and the glue additions the driver relies on: the A5 recorded at
 * stream create and handed to every ASR, and the goodbye kiss (mt_pb_goodbye). */
#include "t.h"
#include "pbfake.h"
#include "mt_util.h"
#include "mt_drv_logic.h"
#include "mt_cfg.h"
#include "mt_pb_entry.h"

static void asr_nop(StreamPtr s, uint16_t ev, MacPtr32 user, uint16_t reason, MacPtr32 icmp) {
    (void)s; (void)ev; (void)user; (void)reason; (void)icmp;
}

#define TRAP_SYNC  0xA004u                 /* _Control */
#define TRAP_ASYNC (0xA004u | MT_DRV_TRAP_ASYNC)

/* ---- routing (who may enter the core) ---- */

static void route_free_gate_runs(void) {
    int16_t cs;
    for (cs = 0; cs <= 60; cs++) {
        T_EQ(mt_drv_route(0, 0, 1, 0, cs), MT_DRV_RUN);    /* async, task time */
        T_EQ(mt_drv_route(0, 0, 0, 0, cs), MT_DRV_RUN);    /* sync, task time */
        T_EQ(mt_drv_route(0, 0, 1, 1, cs), MT_DRV_RUN);    /* async at interrupt time */
    }
}

static void route_preempted_owner_queues_async_refuses_sync(void) {
    T_EQ(mt_drv_route(1, 0, 1, 1, TCPSend), MT_DRV_QUEUE);
    T_EQ(mt_drv_route(1, 0, 1, 0, TCPStatus), MT_DRV_QUEUE);
    T_EQ(mt_drv_route(1, 0, 0, 1, TCPStatus), MT_DRV_REFUSE);   /* even a call that never pends */
    T_EQ(mt_drv_route(1, 0, 0, 1, TCPSend), MT_DRV_REFUSE);
}

static void route_callout_nests(void) {
    T_EQ(mt_drv_route(1, 1, 1, 1, TCPSend), MT_DRV_RUN);       /* async from an ASR */
    T_EQ(mt_drv_route(1, 1, 0, 1, TCPStatus), MT_DRV_RUN);     /* sync, never pends */
    T_EQ(mt_drv_route(1, 1, 0, 0, TCPRelease), MT_DRV_RUN);
    T_EQ(mt_drv_route(1, 1, 0, 0, TCPRcv), MT_DRV_REFUSE);     /* sync in a call-out: would hang */
    T_EQ(mt_drv_route(1, 1, 0, 1, UDPRead), MT_DRV_REFUSE);
}

static void route_sync_pending_at_interrupt_refused(void) {
    static const int16_t pend[] = { TCPPassiveOpen, TCPActiveOpen, TCPSend, TCPNoCopyRcv, TCPRcv,
                                    TCPClose, UDPRead, UDPMultiRead };
    static const int16_t never[] = { TCPCreate, TCPRelease, TCPAbort, TCPStatus, TCPGlobalInfo,
                                     TCPRcvBfrReturn, UDPCreate, UDPWrite, UDPRelease,
                                     UDPBfrReturn, ipctlGetAddr, ipctlEchoICMP, ipctlLAPStats };
    unsigned i;
    for (i = 0; i < sizeof pend / sizeof pend[0]; i++) {
        T_CHECK(mt_drv_may_pend(pend[i]));
        T_EQ(mt_drv_route(0, 0, 0, 1, pend[i]), MT_DRV_REFUSE);
        T_EQ(mt_drv_route(0, 0, 1, 1, pend[i]), MT_DRV_RUN);
    }
    for (i = 0; i < sizeof never / sizeof never[0]; i++) {
        T_CHECK(!mt_drv_may_pend(never[i]));
        T_EQ(mt_drv_route(0, 0, 0, 1, never[i]), MT_DRV_RUN);
    }
}

/* Every csCode in mt_drv_may_pend is driven to inProgress on a live connection / stream (31, 32,
 * 34, 35, 37, 38, 21); UDPMultiRead (29) is listed for MacTCP's sake but answers -4 here (Task 8
 * ruling). The converse (codes outside the list never pend) is the sweep below. */
static void may_pend_covers_every_pending_cscode(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s, s2, s3, u;
    TCPiopb *pb;
    UDPiopb *up;
    uint8_t *rds;
    int16_t r;
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    T_CHECK(s != 0);
    pb = pbt_rcv(&x, s, 10, 0);                                       /* 37 */
    T_EQ(pb->ioResult, inProgress);
    pb = pbt_send(&x, s, "abc", 3, 1);                                /* 34 */
    T_EQ(pb->ioResult, inProgress);
    s2 = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    T_CHECK(s2 != 0);
    pb = pbt_pb(TCPNoCopyRcv, s2);                                    /* 35 */
    rds = (uint8_t *)pbf_alloc(64);
    pb->csParam.receive.rdsPtr = pbf_a(rds);
    pb->csParam.receive.rdsLength = 6;
    pb->csParam.receive.commandTimeoutValue = 10;
    T_EQ(pbt_call(&x, pb), inProgress);
    pb = pbt_pb(TCPClose, s2);                                        /* 38: waits for the FIN's ACK */
    T_EQ(pbt_call(&x, pb), inProgress);
    s3 = pbt_create(&x, 4096, pbf_asr(asr_nop), 0);
    T_CHECK(pbt_open(&x, s3, 0) != NULL);                             /* 32 */
    pb = pbt_pb(TCPPassiveOpen, pbt_create(&x, 4096, pbf_asr(asr_nop), 0));   /* 31 */
    pb->csParam.open.localPort = 4444;
    T_EQ(pbt_call(&x, pb), inProgress);
    u = pbt_ucreate(&x, 0, 4096, 0, 0, NULL);
    up = (UDPiopb *)pbt_pb(UDPRead, u);                               /* 21 */
    r = pbt_call(&x, (TCPiopb *)up);
    T_EQ(r, inProgress);
    up = (UDPiopb *)pbt_pb(UDPMultiRead, u);                          /* 29 */
    T_CHECK(pbt_call(&x, (TCPiopb *)up) != inProgress);
    {
        static const int16_t pend[] = { 31, 32, 34, 35, 37, 38, 21, 29 };
        unsigned i;
        int16_t cs, n = 0;
        for (i = 0; i < sizeof pend / sizeof pend[0]; i++) T_CHECK(mt_drv_may_pend(pend[i]));
        for (cs = -2; cs <= 70; cs++) n += mt_drv_may_pend(cs) != 0;
        T_EQ(n, 8);                                                   /* nothing else is listed */
    }
    T_EQ(pbf_bad, 0);
}

/* The converse: a sweep of every csCode outside the list, with stream 0 and with a
 * live stream, never answers inProgress. */
static void codes_outside_list_never_pend(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s;
    int16_t cs;
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    T_CHECK(s != 0);
    for (cs = 0; cs <= 60; cs++) {
        TCPiopb *pb;
        if (mt_drv_may_pend(cs) || cs == TCPRelease || cs == UDPRelease) continue;
        pb = pbt_pb(cs, 0);
        T_CHECK(pbt_call(&x, pb) != inProgress);
        if (cs == ipctlLAPStats) continue;    /* its stream field is the buffer it fills */
        pb = pbt_pb(cs, s);
        T_CHECK(pbt_call(&x, pb) != inProgress);
    }
}

static void pend_ring_fifo_and_full(void) {
    MtDrvPend q;
    int v[MT_DRV_PENDQ + 2], i;
    memset(&q, 0, sizeof q);
    T_CHECK(mt_drv_pend_pop(&q) == NULL);
    for (i = 0; i < (int)MT_DRV_PENDQ - 1; i++) T_EQ(mt_drv_pend_push(&q, &v[i]), 1);
    T_EQ(mt_drv_pend_push(&q, &v[i]), 0);                 /* one slot stays free */
    T_EQ(q.full, 1);
    T_EQ(q.queued, MT_DRV_PENDQ - 1);
    for (i = 0; i < (int)MT_DRV_PENDQ - 1; i++) T_CHECK(mt_drv_pend_pop(&q) == &v[i]);
    T_CHECK(mt_drv_pend_pop(&q) == NULL);
    for (i = 0; i < 40; i++) {                            /* wraps around */
        T_EQ(mt_drv_pend_push(&q, &v[i % 4]), 1);
        T_CHECK(mt_drv_pend_pop(&q) == &v[i % 4]);
    }
}

/* ---- prefs (MCFG 128) ---- */

static void cfg_full_record(void) {
    static const uint8_t r[] = { 0, 1, 0, 0, 10, 0, 0, 2, 255, 0, 0, 0, 10, 0, 0, 1, 1, 1, 1, 1,
                                 11, 'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm' };
    mt_ifconfig c;
    uint8_t mode = 9;
    T_EQ(mt_cfg_parse(r, sizeof r, &c, &mode), 0);
    T_EQ(c.addr, 0x0A000002u);
    T_EQ(c.mask, 0xFF000000u);
    T_EQ(c.gateway, 0x0A000001u);
    T_EQ(c.dns, 0x01010101u);
    T_CHECK(strcmp(c.domain, "example.com") == 0);
    T_EQ(c.mtu, 1500);
    T_EQ(mode, MT_CFG_MODE_MANUAL);
}

static void cfg_optional_fields(void) {
    static const uint8_t addr_only[] = { 0, 1, 0, 1, 192, 168, 1, 7 };
    static const uint8_t ver_only[] = { 0, 1 };
    static const uint8_t classb[] = { 0, 1, 0, 0, 172, 16, 0, 5, 0, 0, 0, 0 };
    static const uint8_t classa[] = { 0, 1, 0, 0, 10, 1, 2, 3 };
    mt_ifconfig c;
    uint8_t mode;
    T_EQ(mt_cfg_parse(addr_only, sizeof addr_only, &c, &mode), 0);
    T_EQ(c.addr, 0xC0A80107u);
    T_EQ(c.mask, 0xFFFFFF00u);                            /* class C default */
    T_EQ(c.gateway, 0);
    T_EQ(c.dns, 0);
    T_EQ(c.domain[0], 0);
    T_EQ(mode, MT_CFG_MODE_DHCP);
    T_EQ(mt_cfg_parse(ver_only, sizeof ver_only, &c, &mode), 0);
    T_EQ(c.addr, 0);
    T_EQ(c.mask, 0);                                      /* no address: no mask either */
    T_EQ(mt_cfg_parse(classb, sizeof classb, &c, &mode), 0);
    T_EQ(c.mask, 0xFFFF0000u);
    T_EQ(mt_cfg_parse(classa, sizeof classa, &c, &mode), 0);
    T_EQ(c.mask, 0xFF000000u);
}

static void cfg_rejects_bad_records(void) {
    static const uint8_t v2[] = { 0, 2, 0, 0, 10, 0, 0, 2 };
    static const uint8_t shortr[] = { 0 };
    uint8_t longdom[21 + 64];
    uint8_t trunc[] = { 0, 1, 0, 0, 10, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 'a', 'b' };
    mt_ifconfig c;
    uint8_t mode;
    T_EQ(mt_cfg_parse(v2, sizeof v2, &c, &mode), -1);
    T_EQ(c.addr, 0);
    T_EQ(mt_cfg_parse(shortr, sizeof shortr, &c, &mode), -1);
    T_EQ(mt_cfg_parse(NULL, 0, &c, &mode), -1);
    memset(longdom, 'x', sizeof longdom);
    longdom[0] = 0; longdom[1] = 1; longdom[2] = 0; longdom[3] = 0;
    memset(longdom + 4, 0, 16);
    longdom[4] = 10; longdom[7] = 9;
    longdom[20] = 64;                                     /* 64 > 63: ignored, the rest is kept */
    T_EQ(mt_cfg_parse(longdom, sizeof longdom, &c, &mode), 0);
    T_EQ(c.addr, 0x0A000009u);
    T_EQ(c.domain[0], 0);
    T_EQ(mt_cfg_parse(trunc, sizeof trunc, &c, &mode), 0); /* length byte past the end: ignored */
    T_EQ(c.domain[0], 0);
    T_EQ(c.addr, 0x0A000002u);
}

/* ---- glue additions ---- */

static void asr_runs_with_creators_a5(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s, u;
    int i, n = 0;
    pbt_init(&x, 1);
    pbf_a5 = 0x00123450u;                                 /* the app that creates the stream */
    s = pbt_est(&x, pbf_asr(asr_nop), 0x77, &c);
    u = pbt_ucreate(&x, 7777, 4096, pbf_asr(asr_nop), 0x78, NULL);
    T_CHECK(s && u);
    pbf_a5 = 0x00999990u;                                 /* "interrupt time": something else */
    pbt_peer(&x, c.lport, c.ps, c.ms, TF_ACK | TF_PSH, 8192, "hello", 5);   /* DataArrival ASR */
    pbt_udp_in(&x, 999, 7777, "dgram", 5);                                  /* UDPDataArrival */
    for (i = 0; i < pbf_nlog && i < PBF_LOG; i++)
        if (pbf_log[i].kind == 'A') {
            T_EQ(pbf_log[i].a5, 0x00123450u);
            n++;
        }
    T_EQ(n, 2);
    T_EQ(pbf_bad, 0);
}

static void goodbye_forgets_everything_silently(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s, s2, u;
    TCPiopb *rcv, *snd;
    int nlog, f, rst = 0;
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    s2 = pbt_create(&x, 4096, pbf_asr(asr_nop), 0);
    u = pbt_ucreate(&x, 0, 4096, pbf_asr(asr_nop), 0, NULL);
    T_CHECK(s && s2 && u);
    rcv = pbt_rcv(&x, s, 100, 0);
    snd = pbt_send(&x, s, "xyz", 3, 1);
    T_EQ(rcv->ioResult, inProgress);
    T_EQ(snd->ioResult, inProgress);
    nlog = pbf_nlog;
    fake_clear(&x.fl);
    mt_pb_goodbye(x.d);
    T_EQ(pbf_nlog, nlog);                                 /* no completion, no ASR */
    T_EQ(rcv->ioResult, inProgress);                      /* the app's PBs are not touched */
    for (f = 0; f < x.fl.n; f++) {
        tcpv v;
        if (view_tcp(x.fl.f[f], x.fl.len[f], &v) && (v.flags & TF_RST)) rst = 1;
    }
    T_CHECK(rst);                                         /* the live connection was reset */
    T_EQ(pbt_simple(&x, TCPStatus, s), invalidStreamPtr);
    T_EQ(pbt_simple(&x, TCPRelease, s2), invalidStreamPtr);
    {
        UDPiopb *up = (UDPiopb *)pbt_pb(UDPRelease, u);
        T_EQ(pbt_call(&x, (TCPiopb *)up), invalidStreamPtr);
    }
    nlog = pbf_nlog;                                      /* late segments for the old connection */
    pbt_peer(&x, c.lport, c.ps, c.ms + 3, TF_ACK | TF_PSH, 8192, "late", 4);
    T_EQ(pbf_count('A'), 0);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);             /* the table is usable again */
    T_CHECK(s != 0);
    T_EQ(pbf_bad, 0);
}

/* ---- the Control entry (mac/glue/mt_pb_entry.c) over the fake OS ---- */

static MtEntry g_e;
static MtGate  g_g;
static pbt     g_x;

static void entry_setup(void) {
    pbt_init(&g_x, 1);
    memset(&g_g, 0, sizeof g_g);
    mt_entry_init(&g_e, g_x.d, &g_g);
}

static int ncompl(MacPtr32 pb) {
    int i, n = 0;
    for (i = 0; i < pbf_nlog && i < PBF_LOG; i++) n += pbf_log[i].kind == 'C' && pbf_log[i].pb == pb;
    return n;
}

static void entry_runs_completes_once_and_stashes_a5(void) {
    StreamPtr s;
    TCPiopb *pb;
    MacPtr32 a5;
    pbt_conn c;
    entry_setup();
    s = pbt_est(&g_x, pbf_asr(asr_nop), 0, &c);
    pbf_a5 = 0x00345670u;
    pb = pbt_pb(TCPStatus, s);
    T_EQ(mt_entry_control(&g_e, (MtAnyPb *)pb, TRAP_ASYNC, 0), 0);
    T_EQ(pb->ioResult, 0);
    T_EQ(ncompl(pbf_a(pb)), 1);
    a5 = mt_entry_stashed_a5((MtAnyPb *)pb);
    T_EQ(a5, 0x00345670u);
    pb = pbt_pb(TCPRcv, s);                               /* goes pending: no completion yet */
    pb->csParam.receive.commandTimeoutValue = 10;
    pb->csParam.receive.rcvBuff = pbf_a(pbf_alloc(16));
    pb->csParam.receive.rcvBuffLen = 16;
    T_EQ(mt_entry_control(&g_e, (MtAnyPb *)pb, TRAP_ASYNC, 0), 0);
    T_EQ(pb->ioResult, inProgress);
    T_EQ(ncompl(pbf_a(pb)), 0);
    pb = pbt_pb(TCPStatus, 0x123);                        /* error result: completion once */
    T_EQ(mt_entry_control(&g_e, (MtAnyPb *)pb, TRAP_SYNC, 0), invalidStreamPtr);
    T_EQ(ncompl(pbf_a(pb)), 1);
    T_EQ(g_e.n_calls, 3);
    T_EQ(pbf_mask, 0);
    T_EQ(pbf_bad, 0);
}

static void entry_queues_while_preempted_and_drain_runs(void) {
    StreamPtr s;
    TCPiopb *pb[3];
    pbt_conn c;
    int i;
    entry_setup();
    s = pbt_est(&g_x, pbf_asr(asr_nop), 0, &c);
    g_g.depth = 1;                                        /* an owner was preempted */
    for (i = 0; i < 3; i++) {
        pb[i] = pbt_pb(TCPStatus, s);
        T_EQ(mt_entry_control(&g_e, (MtAnyPb *)pb[i], TRAP_ASYNC, 1), 0);
        T_EQ(pb[i]->ioResult, inProgress);
        T_EQ(ncompl(pbf_a(pb[i])), 0);
    }
    T_EQ(g_g.tick_pending, 1);
    T_EQ(g_e.pend.queued, 3);
    mt_entry_drain(&g_e);                                 /* the owner, before it lets go */
    for (i = 0; i < 3; i++) {
        T_EQ(pb[i]->ioResult, 0);
        T_EQ(ncompl(pbf_a(pb[i])), 1);
    }
    T_CHECK(pbf_find('C', pbf_a(pb[0]), 0) < pbf_find('C', pbf_a(pb[2]), 0));   /* FIFO */
    T_EQ(g_e.n_pend_run, 3);
    T_CHECK(pbf_mask_max >= 1);                           /* the ring is touched masked */
    T_EQ(pbf_mask, 0);
    T_EQ(pbf_bad, 0);
}

static void entry_sync_refusals_complete_at_once(void) {
    TCPiopb *pb;
    entry_setup();
    g_g.depth = 1;                                        /* preempted owner: sync cannot wait */
    pb = pbt_pb(TCPStatus, 0);
    T_EQ(mt_entry_control(&g_e, (MtAnyPb *)pb, TRAP_SYNC, 1), MT_DRV_REFUSED);
    T_EQ(pb->ioResult, MT_DRV_REFUSED);
    T_EQ(ncompl(pbf_a(pb)), 1);
    g_g.depth = 0;                                        /* sync TCPRcv at interrupt time */
    pb = pbt_pb(TCPRcv, 0);
    T_EQ(mt_entry_control(&g_e, (MtAnyPb *)pb, TRAP_SYNC, 1), MT_DRV_REFUSED);
    T_EQ(ncompl(pbf_a(pb)), 1);
    T_EQ(g_e.n_refused, 2);
}

/* The pending ring full: an async refusal is not completed inside PBControl but by the owner. */
static void entry_async_refusal_deferred_to_owner(void) {
    TCPiopb *pb[MT_DRV_PENDQ + 1];
    unsigned i;
    entry_setup();
    g_g.depth = 1;
    for (i = 0; i <= MT_DRV_PENDQ; i++) {
        pb[i] = pbt_pb(TCPStatus, 0);
        T_EQ(mt_entry_control(&g_e, (MtAnyPb *)pb[i], TRAP_ASYNC, 1), 0);
        T_EQ(pb[i]->ioResult, inProgress);
    }
    T_EQ(pbf_count('C'), 0);                              /* none inside the callers' PBControl */
    T_EQ(g_e.pend.full, 2);                               /* 15 queued, 2 refused */
    T_EQ(g_e.n_refused, 2);
    mt_entry_drain(&g_e);
    for (i = 0; i < MT_DRV_PENDQ - 1; i++) T_EQ(pb[i]->ioResult, invalidStreamPtr);   /* ran */
    T_EQ(pb[MT_DRV_PENDQ - 1]->ioResult, MT_DRV_REFUSED);
    T_EQ(pb[MT_DRV_PENDQ]->ioResult, MT_DRV_REFUSED);
    T_EQ(pbf_count('C'), (int)MT_DRV_PENDQ + 1);
    T_CHECK(pbf_find('C', pbf_a(pb[MT_DRV_PENDQ - 2]), 0) < pbf_find('C', pbf_a(pb[MT_DRV_PENDQ - 1]), 0));
    T_EQ(pbf_bad, 0);
}

/* An app that retries a refused call from its completion routine: sync retries recurse at most
 * MT_ENTRY_REFUSE_NEST deep; an async refusal is completed by the owner, so its retry runs as a
 * call-out (the retry's own error then completes inline, as IODone would: depth 2, no loop). */
static int g_retry_depth, g_retry_max, g_retries;
static uint16_t g_retry_trap;
static void retry_compl(MtAnyPb *pb) {
    g_retry_depth++;
    if (g_retry_depth > g_retry_max) g_retry_max = g_retry_depth;
    if (pb->hdr.ioResult == MT_DRV_REFUSED && g_retries++ < 50)
        (void)mt_entry_control(&g_e, pb, g_retry_trap, 1);
    g_retry_depth--;
}

static void entry_refusal_retry_is_bounded(void) {
    TCPiopb *pb;
    unsigned i;
    entry_setup();
    g_retry_depth = g_retry_max = g_retries = 0;
    g_retry_trap = TRAP_SYNC;
    pb = pbt_pb(TCPRcv, 0);
    pb->ioCompletion = pbf_compl(retry_compl);
    (void)mt_entry_control(&g_e, (MtAnyPb *)pb, TRAP_SYNC, 1);
    T_EQ(g_retry_max, MT_ENTRY_REFUSE_NEST);
    T_EQ(g_e.n_compl_skipped, 1);
    T_EQ(pb->ioResult, MT_DRV_REFUSED);
    T_EQ(g_e.refuse_nest, 0);

    entry_setup();                                        /* async, ring full, owner busy */
    g_retry_depth = g_retry_max = g_retries = 0;
    g_retry_trap = TRAP_ASYNC;
    g_g.depth = 1;
    for (i = 0; i < MT_DRV_PENDQ - 1; i++)
        (void)mt_entry_control(&g_e, (MtAnyPb *)pbt_pb(TCPStatus, 0), TRAP_ASYNC, 1);
    pb = pbt_pb(TCPStatus, 0);
    pb->ioCompletion = pbf_compl(retry_compl);
    (void)mt_entry_control(&g_e, (MtAnyPb *)pb, TRAP_ASYNC, 1);
    T_EQ(g_retry_max, 0);                                 /* nothing ran inside the call */
    mt_entry_drain(&g_e);                                 /* the retry runs as a call-out: nests */
    T_EQ(g_retry_max, 2);
    T_EQ(g_retries, 1);
    T_EQ(pb->ioResult, invalidStreamPtr);
    T_EQ(g_e.n_compl_skipped, 0);
    T_EQ(pbf_bad, 0);
}

static void entry_killio_answers_zero(void) {
    TCPiopb *pb;
    entry_setup();
    pb = pbt_pb(1, 0);
    T_EQ(mt_entry_control(&g_e, (MtAnyPb *)pb, 0xA006u | MT_DRV_TRAP_IMMED, 0), 0);
    T_EQ(pb->ioResult, 0);
    T_EQ(pbf_count('C'), 0);
}

/* ExitToShell from inside an ASR: the goodbye arrives with the core owned and a call-out active.
 * The ownership is cleared, the app's objects and queued PBs are forgotten silently. */
static void entry_goodbye_from_inside_a_routine(void) {
    StreamPtr s;
    TCPiopb *q, *pb;
    pbt_conn c;
    entry_setup();
    s = pbt_est(&g_x, pbf_asr(asr_nop), 0, &c);
    g_g.depth = 1;
    q = pbt_pb(TCPStatus, s);
    T_EQ(mt_entry_control(&g_e, (MtAnyPb *)q, TRAP_ASYNC, 1), 0);   /* queued, app memory */
    g_x.d->depth = 2;
    pb = pbt_pb(-1, 0);
    T_EQ(mt_entry_control(&g_e, (MtAnyPb *)pb, TRAP_SYNC, 1), 0);
    T_EQ(g_e.n_goodbye, 1);
    T_EQ(g_e.n_goodbye_forced, 1);
    T_EQ(g_e.n_pend_dropped, 1);
    T_EQ(g_g.depth, 0);
    T_EQ(g_x.d->depth, 0);
    T_EQ(g_x.d->in_core, 0);
    mt_entry_drain(&g_e);
    T_EQ(ncompl(pbf_a(q)), 0);                            /* the dropped PB is never completed */
    T_EQ(pbt_simple(&g_x, TCPStatus, s), invalidStreamPtr);
    T_CHECK(pbt_est(&g_x, pbf_asr(asr_nop), 0, &c) != 0); /* usable again */
    T_EQ(pbf_bad, 0);
}

/* Streams and echoes of system-heap clients survive another application's goodbye. */
static void goodbye_keeps_system_heap_clients(void) {
    pbt x;
    pbt_conn c;
    StreamPtr sys, sysu, app;
    pbt_init(&x, 1);
    sys = pbt_est(&x, pbf_asr(asr_nop), 0, &c);           /* "system heap": below the app range */
    sysu = pbt_ucreate(&x, 7001, 4096, 0, 0, NULL);
    pbf_app_lo = pbf_a(pbf_alloc(8));                     /* everything from here on is the app's */
    app = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    T_CHECK(sys && sysu && app);
    mt_pb_goodbye(x.d);
    T_EQ(pbt_simple(&x, TCPStatus, sys), 0);
    T_EQ(pbt_simple(&x, TCPStatus, app), invalidStreamPtr);
    {
        UDPiopb *up = (UDPiopb *)pbt_pb(UDPRelease, sysu);
        T_EQ(pbt_call(&x, (TCPiopb *)up), 0);
    }
    T_EQ(pbf_bad, 0);
}

T_MAIN(route_free_gate_runs, route_preempted_owner_queues_async_refuses_sync, route_callout_nests,
       route_sync_pending_at_interrupt_refused, may_pend_covers_every_pending_cscode,
       pend_ring_fifo_and_full, cfg_full_record, cfg_optional_fields, cfg_rejects_bad_records,
       asr_runs_with_creators_a5, goodbye_forgets_everything_silently,
       codes_outside_list_never_pend, entry_runs_completes_once_and_stashes_a5,
       entry_queues_while_preempted_and_drain_runs, entry_sync_refusals_complete_at_once,
       entry_async_refusal_deferred_to_owner, entry_refusal_retry_is_bounded,
       entry_killio_answers_zero, entry_goodbye_from_inside_a_routine,
       goodbye_keeps_system_heap_clients)
