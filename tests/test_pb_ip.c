/* PB dispatcher, IP / ICMP / address / global-info calls (csCodes 15, 17, 19, 43) and the
 * csCode sweep. Expected values are real MacTCP 2.1 results; each assertion that encodes one
 * cites its case in docs/dev/MACTCP-REFERENCE-MATRIX.md. */
#include "t.h"
#include "pbfake.h"
#include "mt_util.h"

typedef struct { int n; int16_t res[4]; ICMPParamBlock blk[4]; uint8_t data[4][80]; MacPtr32 pbaddr[4]; } notes;
static notes g_nt;

/* The icmpCompletion routine: entered with the driver's own copy of the block (matrix "ICMP
 * echo notify"). The echoed data is only valid during the call, so it is copied here. */
static void echo_notify(MtAnyPb *pb) {
    int k = g_nt.n < 4 ? g_nt.n : 3;
    g_nt.res[k] = pb->hdr.ioResult;
    memcpy(&g_nt.blk[k], &pb->icmp, sizeof(ICMPParamBlock));
    g_nt.pbaddr[k] = pbf_a(pb);
    if (pb->icmp.icmpEchoInfo.echoedData.length) {
        const uint8_t *d = (const uint8_t *)pbf_p(pb->icmp.icmpEchoInfo.echoedData.ptr);
        uint16_t n = pb->icmp.icmpEchoInfo.echoedData.length;
        memcpy(g_nt.data[k], d, n < 80 ? n : 80);
    }
    g_nt.n++;
}

static IPParamBlock *ipb(int16_t cs) { return (IPParamBlock *)pbt_pb(cs, 0); }
static int16_t icall(pbt *x, void *pb) { return pbt_call(x, (TCPiopb *)pb); }

/* An ICMPEcho PB (IPParamBlock view) to `dest` with `len` payload bytes. */
static IPParamBlock *echo_pb(pbt *x, mt_ip4 dest, const void *data, uint16_t len, int16_t tmo,
                             int with_notify) {
    IPParamBlock *pb = ipb(ipctlEchoICMP);
    uint8_t *b = (uint8_t *)pbf_alloc(len ? len : 1);
    (void)x;
    if (len) memcpy(b, data, len);
    pb->csParam.IPEchoPB.dest = dest;
    pb->csParam.IPEchoPB.data.length = len;
    pb->csParam.IPEchoPB.data.ptr = len ? pbf_a(b) : 0;
    pb->csParam.IPEchoPB.timeout = tmo;
    pb->csParam.IPEchoPB.icmpCompletion = with_notify ? pbf_compl(echo_notify) : 0;
    return pb;
}

/* The ICMP echo request in the last frames sent (type 8): fills id/seq, returns payload length or -1. */
static int last_echo_req(pbt *x, mt_ip4 *dst, uint16_t *id, uint16_t *seq, uint8_t *data) {
    int i;
    for (i = x->fl.n - 1; i >= 0; i--) {
        ipv v;
        if (view_ip(x->fl.f[i], x->fl.len[i], &v) && v.proto == 1 && v.len >= 8 && v.payload[0] == 8) {
            *dst = v.dst;
            *id = mt_rd16(v.payload + 4);
            *seq = mt_rd16(v.payload + 6);
            if (v.len > 8) memcpy(data, v.payload + 8, v.len - 8u);
            return v.len - 8;
        }
    }
    return -1;
}

static void get_my_ip_addr(void) {
    pbt x;
    GetAddrParamBlock *pb;
    pbt_init(&x, 1);
    pb = (GetAddrParamBlock *)pbt_pb(ipctlGetAddr, 0);
    T_EQ(icall(&x, pb), 0);                               /* matrix 110 */
    T_EQ(pb->ioResult, 0);
    T_EQ(pb->ourAddress, T_ME);
    T_EQ((uint32_t)pb->ourNetMask, 0xFFFFFF00u);
    T_EQ(pbf_count('C'), 1);
    /* a config change shows at once */
    {
        mt_ifconfig c;
        mt_get_config(x.d->s, &c);
        c.addr = T_ME + 1u;
        c.mask = 0xFF000000u;
        mt_pb_set_config(x.d, &c);
        pb = (GetAddrParamBlock *)pbt_pb(ipctlGetAddr, 0);
        T_EQ(icall(&x, pb), 0);
        T_EQ(pb->ourAddress, T_ME + 1u);
        T_EQ((uint32_t)pb->ourNetMask, 0xFF000000u);
    }
}

static void echo_icmp_roundtrip_and_timeout(void) {
    pbt x;
    IPParamBlock *pb;
    mt_ip4 dst;
    uint16_t id, seq, id2, seq2;
    uint8_t payload[32], back[80];
    int i, n;
    pbt_init(&x, 1);
    memset(&g_nt, 0, sizeof g_nt);
    for (i = 0; i < 32; i++) payload[i] = (uint8_t)(0x40 + i);
    fake_clear(&x.fl);
    pb = echo_pb(&x, T_PEER, payload, 32, 3, 1);
    T_EQ(icall(&x, pb), 0);                               /* matrix A00: our PB completes 0 once sent */
    T_EQ(pb->ioResult, 0);
    T_EQ(g_nt.n, 0);                                      /* the notify comes with the reply */
    n = last_echo_req(&x, &dst, &id, &seq, back);
    T_EQ(n, 32);
    T_EQ(dst, T_PEER);
    T_MEMEQ(back, payload, 32);
    T_EQ(mt_pb_next_deadline_ms(x.d), 3000);              /* timeout is in seconds (matrix A02 note) */
    pbf_now += 120;
    pbt_echo_reply(&x, T_PEER, id, seq, payload, 32);
    T_EQ(g_nt.n, 1);
    T_EQ(g_nt.res[0], 0);                                 /* matrix A00: notify ioResult 0 */
    T_CHECK(g_nt.pbaddr[0] != pbf_a(pb));                 /* the driver's own block, not our PB */
    T_EQ(g_nt.blk[0].icmpEchoInfo.echoRequestOut, 60);    /* ticks: 1000 ms -> 60 */
    T_EQ(g_nt.blk[0].icmpEchoInfo.echoReplyIn, 67);       /* 1120 ms -> 67 */
    T_EQ(g_nt.blk[0].icmpEchoInfo.echoedData.length, 32);
    T_MEMEQ(g_nt.data[0], payload, 32);
    T_EQ(pbf_count('C'), 2);                              /* our PB, then the notify */
    T_EQ(mt_pb_ip_deadline(x.d), MT_NO_DEADLINE);         /* nothing left waiting */
    pbt_echo_reply(&x, T_PEER, id, seq, payload, 32);     /* a duplicate reply is ignored */
    T_EQ(g_nt.n, 1);

    /* no reply: -23035 after the timeout (matrix A02, A03) */
    fake_clear(&x.fl);
    pb = echo_pb(&x, T_PEER, payload, 8, 2, 1);
    T_EQ(icall(&x, pb), 0);
    n = last_echo_req(&x, &dst, &id2, &seq2, back);
    T_EQ(n, 8);
    T_CHECK(seq2 != seq);                                 /* every request has its own sequence number */
    pbt_echo_reply(&x, T_PEER, id2, (uint16_t)(seq2 + 1u), payload, 8);   /* a stray reply */
    pbt_echo_reply(&x, T_GW, id2, seq2, payload, 8);      /* from the wrong host */
    T_EQ(g_nt.n, 1);
    pbf_now += 1999;
    mt_pb_tick(x.d);
    T_EQ(g_nt.n, 1);
    pbf_now += 1;
    mt_pb_tick(x.d);
    T_EQ(g_nt.n, 2);
    T_EQ(g_nt.res[1], icmpEchoTimeoutErr);                /* matrix A02/A03: -23035 */
    T_EQ(g_nt.blk[1].icmpEchoInfo.echoReplyIn, 0);
    T_EQ(pb->ioResult, 0);                                /* our PB stays as it was completed */
    pbt_echo_reply(&x, T_PEER, id2, seq2, payload, 8);    /* a late reply does nothing */
    T_EQ(g_nt.n, 2);
    T_EQ(pbf_bad, 0);
    T_EQ(pbf_irq, 0);
}

static void echo_argument_rules(void) {
    pbt x;
    IPParamBlock *pb, *p[MT_PB_ECHOQ + 1];
    uint8_t big[1600];
    int i;
    pbt_init(&x, 1);
    memset(&g_nt, 0, sizeof g_nt);
    memset(big, 1, sizeof big);
    pb = echo_pb(&x, T_PEER, big, 1473, 3, 1);
    T_EQ(icall(&x, pb), invalidLength);                   /* does not fit one frame */
    T_EQ(mt_pb_ip_deadline(x.d), MT_NO_DEADLINE);
    pb = echo_pb(&x, T_PEER, big, 1472, 3, 1);
    T_EQ(icall(&x, pb), 0);
    pb = echo_pb(&x, T_PEER, big, 5, 0, 1);               /* timeout 0: the default (3 s), not "forever" */
    T_EQ(icall(&x, pb), 0);
    pbf_now += MT_PB_ECHO_TMO_S * 1000u;
    mt_pb_tick(x.d);
    T_EQ(g_nt.n, 2);
    T_EQ(g_nt.res[0], icmpEchoTimeoutErr);
    pbf_now += 1;
    for (i = 0; i <= MT_PB_ECHOQ; i++) {                  /* the waiting table is finite */
        p[i] = echo_pb(&x, T_PEER, big, 4, 60, i != 1);   /* #1 has no notify routine */
        icall(&x, p[i]);
    }
    T_EQ(p[MT_PB_ECHOQ - 1]->ioResult, 0);
    T_EQ(p[MT_PB_ECHOQ]->ioResult, insufficientResources);
    pbf_now += 61000u;
    mt_pb_tick(x.d);
    T_EQ(g_nt.n, 2 + MT_PB_ECHOQ - 1);                    /* all but the one without a routine */
    {   /* the slots are free again */
        IPParamBlock *q = echo_pb(&x, T_PEER, big, 4, 60, 1);
        T_EQ(icall(&x, q), 0);
    }
    T_EQ(pbf_bad, 0);
}

/* The notify routine reissues an echo from inside the call (the slot it runs in is not reused). */
static pbt *g_rx;
static int g_reissued;
static void echo_again(MtAnyPb *pb) {
    IPParamBlock *q = (IPParamBlock *)pbt_pb(ipctlEchoICMP, 0);
    uint8_t d[4] = {1, 2, 3, 4};
    uint8_t *b = (uint8_t *)pbf_alloc(4);
    (void)pb;
    memcpy(b, d, 4);
    q->csParam.IPEchoPB.dest = T_PEER;
    q->csParam.IPEchoPB.data.length = 4;
    q->csParam.IPEchoPB.data.ptr = pbf_a(b);
    q->csParam.IPEchoPB.timeout = 5;
    q->csParam.IPEchoPB.icmpCompletion = 0;
    g_reissued = icall(g_rx, q);
}

static void echo_reissue_from_notify(void) {
    pbt x;
    IPParamBlock *pb;
    mt_ip4 dst;
    uint16_t id, seq;
    uint8_t back[80];
    pbt_init(&x, 1);
    g_rx = &x;
    g_reissued = 99;
    fake_clear(&x.fl);
    pb = echo_pb(&x, T_PEER, "abcd", 4, 3, 0);
    pb->csParam.IPEchoPB.icmpCompletion = pbf_compl(echo_again);
    T_EQ(icall(&x, pb), 0);
    T_CHECK(last_echo_req(&x, &dst, &id, &seq, back) == 4);
    pbt_echo_reply(&x, T_PEER, id, seq, "abcd", 4);
    T_EQ(g_reissued, 0);
    T_EQ(mt_pb_next_deadline_ms(x.d), 5000);              /* the second echo waits */
    T_EQ(pbf_bad, 0);
}

static void get_tcp_global_info_counts_open(void) {
    pbt x;
    TCPiopb *pb;
    StreamPtr idle, conn;
    pbt_conn c;
    MacPtr32 *cdb;
    TCPStats *st;
    TCPParam *p;
    pbt_init(&x, 1);
    idle = pbt_create(&x, 4096, 0, 0);
    conn = pbt_est(&x, 0, 0, &c);
    T_CHECK(idle != 0 && conn != 0);
    T_EQ(mt_tcp_count_open(x.d->s), 1);
    pb = pbt_pb(TCPGlobalInfo, 0);
    T_EQ(pbt_call(&x, pb), 0);                            /* matrix 800 */
    cdb = (MacPtr32 *)pbf_p(pb->csParam.globalInfo.tcpCDBTable[0]);
    st = (TCPStats *)pbf_p(pb->csParam.globalInfo.tcpStatsPtr);
    p = (TCPParam *)pbf_p(pb->csParam.globalInfo.tcpParamPtr);
    T_CHECK(cdb && st && p);
    if (!cdb || !st || !p) return;
    T_EQ(cdb[0], conn);                                   /* the connections come first ... */
    T_EQ(cdb[1], idle);                                   /* ... then the other live streams */
    T_EQ(cdb[2], 0);
    T_EQ(p->tcpMaxConn, 16);
    T_EQ(p->tcpMaxSegSize, 1460);                         /* matrix 800: 1460 (mtu - 40) */
    T_EQ(st->tcpConnAttempts, 1);                         /* mt_stats.tcp_active_opens */
    T_EQ(st->tcpOutputPkts, mt_get_stats(x.d->s)->tcp_out);
    T_EQ(st->tcpInputPkts, mt_get_stats(x.d->s)->tcp_in);
    T_EQ(pbt_simple(&x, TCPAbort, conn), 0);
    T_EQ(mt_tcp_count_open(x.d->s), 0);
    pb = pbt_pb(TCPGlobalInfo, 0);
    T_EQ(pbt_call(&x, pb), 0);
    cdb = (MacPtr32 *)pbf_p(pb->csParam.globalInfo.tcpCDBTable[0]);
    T_CHECK(cdb[0] == idle || cdb[0] == conn);
    T_EQ(cdb[2], 0);
}

static void icmp_stats_and_lap_stats_fields_from_mt_stats(void) {
    pbt x;
    IPParamBlock *pb;
    LAPStats ls, empty;
    uint8_t *mem;
    const uint8_t *mac;
    const mt_stats *ms;
    uint32_t out0;
    mt_ip4 dst;
    uint16_t id, seq;
    uint8_t back[80];
    pbt_init(&x, 1);
    mem = (uint8_t *)pbf_alloc(sizeof(LAPStats) + 2);
    pb = ipb(ipctlLAPStats);
    pb->csParam.LAPStatsPB.lapStatsPtr = pbf_a(mem + 1);  /* unaligned buffer is fine */
    T_EQ(icall(&x, pb), 0);                               /* matrix 160 */
    memcpy(&ls, mem + 1, sizeof ls);
    T_EQ(ls.ifType, 6);                                   /* Ethernet */
    T_EQ(ls.ifMaxMTU, 1500);
    T_EQ(ls.ifSpeed, 10000000);
    T_EQ(ls.ifPhyAddrLength, 6);
    T_EQ(ls.slotNumber, -1);
    mac = (const uint8_t *)pbf_p(ls.ifPhysicalAddress);
    T_CHECK(mac != NULL);
    if (mac) T_MEMEQ(mac, T_ME_MAC, 6);                   /* from the config the glue was given */
    T_CHECK(pbf_p(ls.AddrXlation) != NULL);               /* matrix 160: AddrXlation set */
    T_CHECK(pbf_p(ls.ifString) != NULL);
    pb = ipb(ipctlLAPStats);                              /* no buffer */
    T_EQ(icall(&x, pb), invalidBufPtr);
    {   /* a smaller MTU in the config shows */
        mt_ifconfig c;
        mt_get_config(x.d->s, &c);
        c.mtu = 576;
        mt_pb_set_config(x.d, &c);
        pb = ipb(ipctlLAPStats);
        memset(mem, 0, sizeof(LAPStats) + 2);
        pb->csParam.LAPStatsPB.lapStatsPtr = pbf_a(mem);
        T_EQ(icall(&x, pb), 0);
        memcpy(&ls, mem, sizeof ls);
        T_EQ(ls.ifMaxMTU, 576);
    }
    /* the statistics come from the core's counters: echo traffic moves them, nothing else does */
    ms = mt_get_stats(x.d->s);
    out0 = ms->icmp_out;
    memset(&empty, 0, sizeof empty);
    fake_clear(&x.fl);
    T_EQ(icall(&x, echo_pb(&x, T_PEER, "xy", 2, 3, 0)), 0);
    T_EQ(ms->icmp_out, out0 + 1);
    T_CHECK(last_echo_req(&x, &dst, &id, &seq, back) == 2);
    pbt_echo_reply(&x, T_PEER, id, seq, "xy", 2);
    T_EQ(ms->icmp_in, 1);
    {   /* TCPGlobalInfo's counters are the core's tcp_* counters, nothing of the echo */
        TCPiopb *gi = pbt_pb(TCPGlobalInfo, 0);
        TCPStats *st;
        T_EQ(pbt_call(&x, gi), 0);
        st = (TCPStats *)pbf_p(gi->csParam.globalInfo.tcpStatsPtr);
        T_EQ(st->tcpOutputPkts, ms->tcp_out);
        T_EQ(st->tcpRetransPkts, ms->tcp_rexmt);
        T_EQ(st->tcpConnAttempts, 0);
    }
}

static void unknown_cscode_returns_matrix_error(void) {
    pbt x;
    int16_t cs;
    pbt_init(&x, 1);
    for (cs = 0; cs <= 60; cs++) {
        TCPiopb *pb;
        int16_t want, r;
        if (cs == ipctlEchoICMP || cs == ipctlLAPStats || cs == TCPGlobalInfo) continue;
        if ((cs >= 21 && cs <= 24) || (cs >= 28 && cs <= 29) || (cs >= 31 && cs <= 42) || cs == 50)
            want = invalidStreamPtr;                      /* matrix 120: 21-24, 28, 29, 31-42, 50 */
        else if (cs == 20 || cs == 27 || cs == 30)
            want = invalidLength;                         /* matrix 120: 20, 27, 30 */
        else if (cs == 1 || cs == 9 || cs == 10 || cs == 11 || cs == 14 || cs == 15 || cs == 16 ||
                 cs == 18 || cs == 25 || cs == 26 || cs == 44)
            want = 0;
        else
            want = -4;                                    /* 0, 2-8, 12, 13, 45-49, 51-60 */
        pb = pbt_pb(cs, 0);
        r = pbt_call(&x, pb);
        if (r != want) fprintf(stderr, "csCode %d: %d, want %d\n", cs, r, want);
        T_EQ(r, want);
        T_EQ(pb->ioResult, want);
    }
    T_EQ(pbf_bad, 0);
}

static void unconfigured_ip_calls_per_matrix(void) {
    pbt x;
    int16_t cs;
    pbt_init(&x, 0);
    for (cs = 0; cs <= 60; cs++) {                        /* matrix: csCode sweep 0-60: -28 for every code */
        TCPiopb *pb = pbt_pb(cs, 0);
        T_EQ(pbt_call(&x, pb), -28);
        T_EQ(pb->ioResult, -28);
    }
    T_EQ(icall(&x, ipb(ipctlEchoICMP)), -28);             /* matrix U01-U12, 800, 160 */
    T_EQ(icall(&x, ipb(ipctlLAPStats)), -28);
    T_EQ(icall(&x, pbt_pb(ipctlGetAddr, 0)), -28);
    T_EQ(pbf_count('C'), 0);                              /* no completion routine runs */
    T_EQ(mt_pb_next_deadline_ms(x.d), MT_NO_DEADLINE);
    {   /* an address arrives: the same calls work */
        mt_ifconfig c;
        GetAddrParamBlock *g;
        memset(&c, 0, sizeof c);
        c.addr = T_ME;
        c.mask = 0xFFFFFF00u;
        c.mtu = 1500;
        mt_pb_set_config(x.d, &c);
        g = (GetAddrParamBlock *)pbt_pb(ipctlGetAddr, 0);
        T_EQ(icall(&x, g), 0);
        T_EQ(g->ourAddress, T_ME);
    }
}

/* PBs in EXACT-size heap blocks (ASan sees any byte read or written past the documented size). */
static int16_t exact_call(pbt *x, const void *pb, size_t n) {
    void *h = malloc(n);
    int16_t r;
    memcpy(h, pb, n);
    r = mt_pb_control(x->d, (MtAnyPb *)h);
    free(h);
    return r;
}

static void exact_size_pbs(void) {
    pbt x;
    IPParamBlock *pb;
    uint8_t *mem;
    pbt_init(&x, 1);
    memset(&g_nt, 0, sizeof g_nt);
    T_EQ(sizeof(GetAddrParamBlock), 36);
    T_EQ(sizeof(IPParamBlock), 54);
    T_EQ(exact_call(&x, pbt_pb(ipctlGetAddr, 0), sizeof(GetAddrParamBlock)), 0);
    mem = (uint8_t *)pbf_alloc(sizeof(LAPStats));
    pb = ipb(ipctlLAPStats);
    pb->csParam.LAPStatsPB.lapStatsPtr = pbf_a(mem);
    T_EQ(exact_call(&x, pb, sizeof(IPParamBlock)), 0);
    pb = ipb(ipctlLAPStats);
    T_EQ(exact_call(&x, pb, sizeof(IPParamBlock)), invalidBufPtr);
    pb = echo_pb(&x, T_PEER, "abcd", 4, 3, 1);
    T_EQ(exact_call(&x, pb, sizeof(IPParamBlock)), 0);   /* the 54-byte echo PB: no over-read */
    pb = echo_pb(&x, T_PEER, "abcd", 4, 3, 1);
    pb->csParam.IPEchoPB.userDataPtr = 0x1234;
    T_EQ(exact_call(&x, pb, sizeof(IPParamBlock)), 0);
    pbf_now += 3000;
    mt_pb_tick(x.d);
    T_EQ(g_nt.n, 2);                                      /* both notify (timeout), from the driver's block */
    T_EQ(g_nt.blk[0].icmpEchoInfo.userDataPtr, 0);        /* not carried over from the app's PB */
    T_EQ(pbf_bad, 0);
}

T_MAIN(get_my_ip_addr, echo_icmp_roundtrip_and_timeout, echo_argument_rules,
       echo_reissue_from_notify, get_tcp_global_info_counts_open,
       icmp_stats_and_lap_stats_fields_from_mt_stats, unknown_cscode_returns_matrix_error,
       unconfigured_ip_calls_per_matrix, exact_size_pbs)
