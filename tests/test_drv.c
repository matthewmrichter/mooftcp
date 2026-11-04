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

/* Boot decision (mt_drv_cfg_kind): the full build runs mode word 1 as DHCP (address = hint, also 0);
 * MoofTCP Lite (MT_NO_DHCP) ignores the mode word and runs the record's address as a manual setting. */
static void cfg_kind_mode_word(void) {
    static const uint8_t dhcp_hint[] = { 0, 1, 0, 1, 192, 168, 1, 7, 255, 255, 255, 0, 192, 168, 1, 1 };
    static const uint8_t dhcp_none[] = { 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 192, 168, 1, 1 };
    static const uint8_t manual[] = { 0, 1, 0, 0, 192, 168, 1, 7 };
    mt_ifconfig c;
    uint8_t mode;
    T_EQ(mt_cfg_parse(dhcp_hint, sizeof dhcp_hint, &c, &mode), 0);
    T_EQ(mode, MT_CFG_MODE_DHCP);                         /* the parser reports it in both variants */
#ifdef MT_NO_DHCP
    T_EQ(mt_drv_cfg_kind(1, mode, c.addr), MT_DRV_CFG_MANUAL);   /* Lite: the address runs statically */
    T_EQ(c.addr, 0xC0A80107u);
    T_EQ(c.gateway, 0xC0A80101u);
#else
    T_EQ(mt_drv_cfg_kind(1, mode, c.addr), MT_DRV_CFG_DHCP);
#endif
    T_EQ(mt_cfg_parse(dhcp_none, sizeof dhcp_none, &c, &mode), 0);
    T_EQ(c.addr, 0);
#ifdef MT_NO_DHCP
    T_EQ(mt_drv_cfg_kind(1, mode, c.addr), MT_DRV_CFG_NONE);     /* Lite: unconfigured, no lease wait */
#else
    T_EQ(mt_drv_cfg_kind(1, mode, c.addr), MT_DRV_CFG_DHCP);
#endif
    T_EQ(mt_cfg_parse(manual, sizeof manual, &c, &mode), 0);
    T_EQ(mt_drv_cfg_kind(1, mode, c.addr), MT_DRV_CFG_MANUAL);
    T_EQ(mt_drv_cfg_kind(1, mode, 0), MT_DRV_CFG_NONE);
    T_EQ(mt_drv_cfg_kind(0, MT_CFG_MODE_DHCP, 0), MT_DRV_CFG_NONE);   /* no file / no record */
    T_EQ(mt_drv_cfg_kind(0, 0, 0xC0A80107u), MT_DRV_CFG_NONE);
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
    pbf_gate = &g_g;                                      /* irq_save/restore move g_g.depth */
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
    pbf_app_hi = PBF_BASE + PBF_SIZE;                     /* the fake procs are system code here */
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

/* ---- final review fix 1: a call-out is only a call at the recorded gate depth ---- */

static void callout_needs_recorded_gate_depth(void) {
    T_CHECK(mt_drv_callout(1, 1, 1));                     /* the routine itself calls */
    T_CHECK(mt_drv_callout(2, 2, 2));                     /* a routine inside a routine */
    T_CHECK(!mt_drv_callout(2, 1, 1));                    /* the routine is mid core call */
    T_CHECK(!mt_drv_callout(1, 0, 1));                    /* no routine running */
    T_CHECK(!mt_drv_callout(0, 1, 1));
}

/* An owner runs a completion (gate depth 1); the routine issues TCPSend (a legit call-out: it
 * runs); while that TCPSend is inside the core (the frame goes out), an "interrupt" calls the
 * driver: async TCPRcv is queued, sync TCPStatus refused, neither enters the core. */
static StreamPtr g_fs;
static TCPiopb *g_fsend, *g_frcv, *g_fstat;
static int16_t g_fsend_r, g_frcv_r, g_fstat_r;
static int g_fhooked;
static void preempt_hook(void) {
    if (g_fhooked++) return;
    g_frcv = pbt_pb(TCPRcv, g_fs);
    g_frcv->csParam.receive.commandTimeoutValue = 10;
    g_frcv->csParam.receive.rcvBuff = pbf_a(pbf_alloc(16));
    g_frcv->csParam.receive.rcvBuffLen = 16;
    g_frcv_r = mt_entry_control(&g_e, (MtAnyPb *)g_frcv, TRAP_ASYNC, 1);
    g_fstat = pbt_pb(TCPStatus, g_fs);
    g_fstat_r = mt_entry_control(&g_e, (MtAnyPb *)g_fstat, TRAP_SYNC, 1);
}
static void send_from_completion(MtAnyPb *pb) {
    wdsEntry *w = (wdsEntry *)pbf_alloc(2 * sizeof(wdsEntry));
    uint8_t *data = (uint8_t *)pbf_alloc(4);
    (void)pb;
    memcpy(data, "ping", 4);
    w[0].length = 4;
    w[0].ptr = pbf_a(data);
    g_fsend = pbt_pb(TCPSend, g_fs);
    g_fsend->csParam.send.wdsPtr = pbf_a(w);
    g_fsend->csParam.send.pushFlag = 1;
    fake_tx_hook = preempt_hook;
    g_fsend_r = mt_entry_control(&g_e, (MtAnyPb *)g_fsend, TRAP_ASYNC, 0);
    fake_tx_hook = NULL;
}

static void preemption_inside_nested_core_call_is_not_a_callout(void) {
    pbt_conn c;
    TCPiopb *trig;
    entry_setup();
    g_fs = pbt_est(&g_x, pbf_asr(asr_nop), 0, &c);
    T_CHECK(g_fs != 0);
    g_fhooked = 0;
    fake_clear(&g_x.fl);
    g_g.depth = 1;                                        /* owner T: tick / after callback */
    trig = pbt_pb(TCPStatus, g_fs);
    trig->ioCompletion = pbf_compl(send_from_completion);
    mt_pb_call_complete(g_x.d, (MtAnyPb *)trig, 0);       /* as mt_entry_drain does */
    T_EQ(g_fhooked, 1);                                   /* the frame did go out */
    T_EQ(g_fsend_r, 0);                                   /* the routine's own call ran */
    T_EQ(g_fsend->ioResult, inProgress);                  /* (waits for the ACK) */
    T_EQ(g_frcv_r, 0);
    T_EQ(g_frcv->ioResult, inProgress);
    T_EQ(g_e.pend.queued, 1);                             /* queued, not run */
    T_EQ(g_fstat_r, MT_DRV_REFUSED);                      /* sync: refused, not run */
    T_EQ(g_fstat->ioResult, MT_DRV_REFUSED);
    T_EQ(g_g.depth, 1);
    T_EQ(g_x.d->depth, 0);
    mt_entry_drain(&g_e);                                 /* the owner runs it afterwards */
    T_EQ(g_e.n_pend_run, 1);
    T_EQ(g_frcv->ioResult, inProgress);                   /* a read pending on the stream now */
    T_EQ(pbf_bad, 0);
}

/* ---- final review fix 2: a chained 1-byte reader has bounded nesting ---- */

static uint8_t g_rd[4096];
static int g_rdn, g_rdepth, g_rdmax;
static void chained_read(MtAnyPb *pb) {
    g_rdepth++;
    if (g_rdepth > g_rdmax) g_rdmax = g_rdepth;
    if (pb->hdr.ioResult == 0 && pb->tcp.csParam.receive.rcvBuffLen == 1) {
        const uint8_t *b = (const uint8_t *)pbf_p(pb->tcp.csParam.receive.rcvBuff);
        if (g_rdn < (int)sizeof g_rd) g_rd[g_rdn] = b[0];
        g_rdn++;
        pb->tcp.csParam.receive.rcvBuffLen = 1;           /* re-issue the same PB */
        pb->tcp.csParam.receive.commandTimeoutValue = 0;
        (void)pbt_call(&g_x, (TCPiopb *)pb);
    }
    g_rdepth--;
}

static void chained_one_byte_reader_is_bounded(void) {
    pbt_conn c;
    StreamPtr s;
    TCPiopb *pb;
    uint8_t data[2800];
    int i, ticks = 0;
    pbt_init(&g_x, 1);
    s = pbt_est(&g_x, pbf_asr(asr_nop), 0, &c);
    T_CHECK(s != 0);
    for (i = 0; i < (int)sizeof data; i++) data[i] = (uint8_t)(i * 7 + i / 251);
    pbt_peer(&g_x, c.lport, c.ps, c.ms, TF_ACK | TF_PSH, 8192, data, 1400);
    pbt_peer(&g_x, c.lport, c.ps + 1400, c.ms, TF_ACK | TF_PSH, 8192, data + 1400, 1400);
    g_rdn = g_rdepth = g_rdmax = 0;
    pb = pbt_pb(TCPRcv, s);
    pb->ioCompletion = pbf_compl(chained_read);
    pb->csParam.receive.rcvBuff = pbf_a(pbf_alloc(4));
    pb->csParam.receive.rcvBuffLen = 1;
    (void)pbt_call(&g_x, pb);
    T_CHECK(g_rdmax <= MT_PB_INLINE_MAX);
    T_CHECK(g_rdn < (int)sizeof data);                    /* the rest waits for the tick */
    T_EQ(mt_pb_next_deadline_ms(g_x.d), 0);               /* ...which is due at once */
    while (g_rdn < (int)sizeof data && ticks < 100) {
        mt_pb_tick(g_x.d);
        ticks++;
    }
    T_EQ(g_rdn, (int)sizeof data);
    T_CHECK(memcmp(g_rd, data, sizeof data) == 0);        /* every byte, in order */
    T_CHECK(g_rdmax <= MT_PB_INLINE_MAX);
    T_CHECK(ticks <= 3);                                  /* the tick serves iteratively */
    T_EQ(pb->ioResult, inProgress);                       /* the last read waits for more data */
    T_EQ(pbf_bad, 0);
}

/* ---- re-review: the entry's own inline completion is bounded too ----
 * A 4 KB reader empties the receive buffer with every read, so each read completes inline in the
 * entry (result 0). The read's window update goes out through link.tx; the "driver" then delivers
 * the next segment synchronously, and the gate's outermost leave runs it before the entry's
 * completion: the app finds data again on every re-issue. */
#define BIG_CHUNK 2920              /* two full segments: a 4 KB window drops below an MSS */
#define BIG_N     12
static uint8_t g_big[BIG_CHUNK * BIG_N], g_bigout[BIG_CHUNK * BIG_N];
static uint32_t g_bin, g_bout;
static int g_barmed, g_bhook, g_bdepth, g_bmax, g_bcompl, g_bissued, g_bbad;
static pbt_conn g_bc;
static mt_tcp *g_bt;                                     /* the reader's core TCB */
static int big_empty(void) {
    mt_tcp_info in;
    mt_tcp_get_info(g_bt, &in);
    return in.rcv_avail == 0;
}
static void big_tx(void) { g_barmed = 1; }
static void big_inject(void) {
    int was = g_bhook;
    g_bhook = 1;                              /* no injection from inside an injection */
    pbt_peer(&g_x, g_bc.lport, g_bc.ps + g_bin, g_bc.ms, TF_ACK, 8192, g_big + g_bin, 1460);
    pbt_peer(&g_x, g_bc.lport, g_bc.ps + g_bin + 1460, g_bc.ms, TF_ACK | TF_PSH, 8192, g_big + g_bin + 1460, 1460);
    g_bin += BIG_CHUNK;
    g_bhook = was;
}
static void big_leave(void) {
    if (!g_barmed || g_bhook || g_bin >= sizeof g_big) return;
    if (!big_empty()) return;                 /* the next segments come after a read emptied it */
    g_barmed = 0;
    big_inject();
}
static void big_reader(MtAnyPb *pb) {
    uint16_t n = pb->tcp.csParam.receive.rcvBuffLen;
    g_bdepth++;
    if (g_bdepth > g_bmax) g_bmax = g_bdepth;
    g_bcompl++;
    if (pb->hdr.ioResult != 0 || g_bout + n > sizeof g_bigout) g_bbad++;
    else {
        memcpy(g_bigout + g_bout, pbf_p(pb->tcp.csParam.receive.rcvBuff), n);
        g_bout += n;
    }
    if (g_bout < sizeof g_bigout) {                       /* re-issue from the completion */
        pb->tcp.csParam.receive.rcvBuffLen = 4096;
        g_bissued++;
        (void)mt_entry_control(&g_e, pb, TRAP_ASYNC, 0);
    }
    g_bdepth--;
}

static void chained_buffer_emptying_reader_is_bounded(void) {
    TCPiopb *pb;
    StreamPtr s;
    unsigned i;
    int loops = 0;
    entry_setup();
    s = pbt_create(&g_x, 4096, pbf_asr(asr_nop), 0);     /* MacTCP's minimum buffer */
    T_EQ(pbt_connect(&g_x, s, &g_bc), 0);
    for (i = 0; i < g_x.d->ntcp; i++) if (g_x.d->tcp[i].handle == s) g_bt = g_x.d->tcp[i].t;
    for (i = 0; i < sizeof g_big; i++) g_big[i] = (uint8_t)(i * 13 + i / 977);
    g_bin = g_bout = 0;
    g_barmed = g_bhook = g_bdepth = g_bmax = g_bcompl = g_bbad = 0;
    big_inject();                                         /* data waiting for the first read */
    fake_tx_hook = big_tx;
    pbf_leave_hook = big_leave;
    pb = pbt_pb(TCPRcv, s);
    pb->ioCompletion = pbf_compl(big_reader);
    pb->csParam.receive.rcvBuff = pbf_a(pbf_alloc(4096));
    pb->csParam.receive.rcvBuffLen = 4096;
    g_bissued = 1;
    (void)mt_entry_control(&g_e, (MtAnyPb *)pb, TRAP_ASYNC, 0);
    while (g_bout < sizeof g_bigout && loops++ < 200) {  /* if the chain ever waits for data */
        if (g_bin < sizeof g_big && big_empty()) big_inject();
        mt_pb_tick(g_x.d);
        mt_entry_drain(&g_e);
    }
    fake_tx_hook = NULL;
    pbf_leave_hook = NULL;
    T_EQ(g_bout, sizeof g_bigout);
    T_CHECK(memcmp(g_bigout, g_big, sizeof g_big) == 0);  /* every byte, in order */
    T_EQ(g_bbad, 0);
    T_EQ(g_bcompl, g_bissued);                            /* each completion exactly once */
    T_CHECK(g_bmax <= MT_PB_INLINE_MAX + 1);
    T_CHECK(g_bmax >= MT_PB_INLINE_MAX);                  /* the scenario did chain to the limit
                                                             (old code: one level per round) */
    T_EQ(g_x.d->depth, 0);
    T_EQ(pbf_bad, 0);
}

/* The entry's own deferral: any call that completes inline (TCPStatus here), re-issued from its
 * completion 20 times, nests at most MT_PB_INLINE_MAX deep; deferred completions run from the
 * outermost frame, each exactly once (one PB re-issued: the order is the issue order). */
static int g_sdepth, g_smax, g_sn, g_sbad;
static void status_chain(MtAnyPb *pb) {
    g_sdepth++;
    if (g_sdepth > g_smax) g_smax = g_sdepth;
    if (pb->hdr.ioResult != 0) g_sbad++;
    if (++g_sn < 20) (void)mt_entry_control(&g_e, pb, TRAP_ASYNC, 0);   /* re-issue the same PB */
    g_sdepth--;
}

static void entry_inline_completions_are_bounded(void) {
    pbt_conn c;
    StreamPtr s;
    TCPiopb *pb;
    entry_setup();
    s = pbt_est(&g_x, pbf_asr(asr_nop), 0x55, &c);
    g_sdepth = g_smax = g_sn = g_sbad = 0;
    pb = pbt_pb(TCPStatus, s);
    pb->ioCompletion = pbf_compl(status_chain);
    (void)mt_entry_control(&g_e, (MtAnyPb *)pb, TRAP_ASYNC, 0);
    T_EQ(g_sn, 20);                                       /* every completion ran exactly once */
    T_EQ(g_sbad, 0);
    T_EQ(ncompl(pbf_a(pb)), 20);
    T_CHECK(g_smax <= MT_PB_INLINE_MAX);
    T_CHECK(g_e.n_compl_deferred > 0);
    T_EQ(g_x.d->depth, 0);
    T_EQ(pbf_bad, 0);
}

/* Minor (a): a system-heap PB whose completion routine is application code is the app's. */
static void goodbye_drops_pbs_with_app_completion(void) {
    StreamPtr s;
    TCPiopb *rcv, *q;
    pbt_conn c;
    entry_setup();
    pbf_app_hi = PBF_BASE + PBF_SIZE;                     /* procs are system code for now */
    s = pbt_est(&g_x, 0, 0, &c);                          /* system buffer, no ASR */
    rcv = pbt_rcv(&g_x, s, 100, 0);                       /* a system-heap PB pending on it */
    T_EQ(rcv->ioResult, inProgress);
    g_g.depth = 1;
    q = pbt_pb(TCPStatus, s);                             /* a system-heap PB queued by preemption */
    T_EQ(mt_entry_control(&g_e, (MtAnyPb *)q, TRAP_ASYNC, 1), 0);
    g_g.depth = 0;
    pbf_app_lo = PBF_BASE + PBF_SIZE;                     /* no fake memory is the app's ... */
    pbf_app_hi = 0xFFFFFFFFu;                             /* ... but the completion routines are */
    T_EQ(mt_entry_control(&g_e, (MtAnyPb *)pbt_pb(-1, 0), TRAP_SYNC, 0), 0);
    T_EQ(g_e.n_pend_dropped, 1);
    T_EQ(rcv->ioResult, inProgress);                      /* silently */
    T_EQ(pbt_simple(&g_x, TCPStatus, s), invalidStreamPtr);
    mt_entry_drain(&g_e);
    T_EQ(ncompl(pbf_a(q)), 0);
    T_EQ(pbf_bad, 0);
}

/* ---- final review fix 5: a stream with the app's ASR or a pending app PB is the app's ---- */

static void goodbye_drops_streams_holding_app_routines_or_pbs(void) {
    pbt x;
    pbt_conn c;
    StreamPtr a, b, cc, u;
    TCPiopb *rcv;
    UDPiopb *up;
    pbt_init(&x, 1);
    pbf_app_hi = PBF_BASE + PBF_SIZE;                     /* procs outside the app for now */
    a = pbt_est(&x, 0, 0, &c);                            /* system buffers, no ASR */
    b = pbt_est(&x, 0, 0, &c);
    cc = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    u = pbt_ucreate(&x, 7002, 4096, 0, 0, NULL);
    T_CHECK(a && b && cc && u);
    pbf_app_lo = pbf_a(pbf_alloc(8));                     /* from here on: the app's memory */
    rcv = pbt_rcv(&x, a, 100, 0);                         /* an app PB pending on stream a */
    T_EQ(rcv->ioResult, inProgress);
    up = (UDPiopb *)pbt_pb(UDPRead, u);                   /* and on the UDP stream */
    T_EQ(pbt_call(&x, (TCPiopb *)up), inProgress);
    pbf_app_hi = 0xFFFFFFFFu;                             /* stream cc's ASR is app code now */
    mt_pb_goodbye(x.d);
    T_EQ(rcv->ioResult, inProgress);                      /* silently: no completion */
    T_EQ(up->ioResult, inProgress);
    T_EQ(pbt_simple(&x, TCPStatus, a), invalidStreamPtr);
    T_EQ(pbt_simple(&x, TCPStatus, cc), invalidStreamPtr);
    T_EQ(pbt_simple(&x, TCPStatus, b), 0);                /* nothing of the app's: kept */
    up = (UDPiopb *)pbt_pb(UDPRelease, u);
    T_EQ(pbt_call(&x, (TCPiopb *)up), invalidStreamPtr);
    T_EQ(pbf_bad, 0);
}

T_MAIN(route_free_gate_runs, route_preempted_owner_queues_async_refuses_sync, route_callout_nests,
       route_sync_pending_at_interrupt_refused, may_pend_covers_every_pending_cscode,
       pend_ring_fifo_and_full, cfg_full_record, cfg_optional_fields, cfg_kind_mode_word, cfg_rejects_bad_records,
       asr_runs_with_creators_a5, goodbye_forgets_everything_silently,
       codes_outside_list_never_pend, entry_runs_completes_once_and_stashes_a5,
       entry_queues_while_preempted_and_drain_runs, entry_sync_refusals_complete_at_once,
       entry_async_refusal_deferred_to_owner, entry_refusal_retry_is_bounded,
       entry_killio_answers_zero, entry_goodbye_from_inside_a_routine,
       goodbye_keeps_system_heap_clients, callout_needs_recorded_gate_depth,
       preemption_inside_nested_core_call_is_not_a_callout, chained_one_byte_reader_is_bounded,
       goodbye_drops_streams_holding_app_routines_or_pbs,
       chained_buffer_emptying_reader_is_bounded, goodbye_drops_pbs_with_app_completion,
       entry_inline_completions_are_bounded)
