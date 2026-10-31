/* test_drv.c - Task 10: the driver entry's pure decisions (mac/glue/mt_drv_logic.h), the prefs
 * parser (mac/glue/mt_cfg.h), and the glue additions the driver relies on: the A5 recorded at
 * stream create and handed to every ASR, and the goodbye kiss (mt_pb_goodbye). */
#include "t.h"
#include "pbfake.h"
#include "mt_util.h"
#include "mt_drv_logic.h"
#include "mt_cfg.h"

static void asr_nop(StreamPtr s, uint16_t ev, MacPtr32 user, uint16_t reason, MacPtr32 icmp) {
    (void)s; (void)ev; (void)user; (void)reason; (void)icmp;
}

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

/* Every csCode whose glue path can return inProgress is in mt_drv_may_pend: drive each one on a
 * live connection / stream and check the result against the list. */
static void may_pend_covers_every_pending_cscode(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s, u;
    TCPiopb *pb;
    UDPiopb *up;
    int16_t r;
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    T_CHECK(s != 0);
    pb = pbt_rcv(&x, s, 10, 0);
    T_EQ(pb->ioResult, inProgress);
    T_CHECK(mt_drv_may_pend(TCPRcv));
    pb = pbt_send(&x, s, "abc", 3, 1);
    T_EQ(pb->ioResult, inProgress);
    T_CHECK(mt_drv_may_pend(TCPSend));
    u = pbt_ucreate(&x, 0, 4096, 0, 0, NULL);
    up = (UDPiopb *)pbt_pb(UDPRead, u);
    r = pbt_call(&x, (TCPiopb *)up);
    T_EQ(r, inProgress);
    T_CHECK(mt_drv_may_pend(UDPRead));
    T_EQ(pbf_bad, 0);
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

T_MAIN(route_free_gate_runs, route_preempted_owner_queues_async_refuses_sync, route_callout_nests,
       route_sync_pending_at_interrupt_refused, may_pend_covers_every_pending_cscode,
       pend_ring_fifo_and_full, cfg_full_record, cfg_optional_fields, cfg_rejects_bad_records,
       asr_runs_with_creators_a5, goodbye_forgets_everything_silently)
