/* PB dispatcher, UDP calls (csCodes 20..29). Expected values are real MacTCP 2.1 results; each
 * assertion that encodes one cites its case in docs/dev/MACTCP-REFERENCE-MATRIX.md (raw lines in
 * docs/dev/probe/mactcp-configured.out). */
#include "t.h"
#include "pbfake.h"
#include "mt_util.h"

#define PEERPORT 17007u

static UDPiopb *upb(int16_t cs, StreamPtr s) { return (UDPiopb *)pbt_pb(cs, s); }
static int16_t ucall(pbt *x, UDPiopb *pb) { return pbt_call(x, (TCPiopb *)pb); }

static int g_udp_asr, g_icmp_asr, g_asr_reentry;
static StreamPtr g_asr_stream;
static MacPtr32 g_asr_user;
static ICMPReport g_rep;
static pbt *g_x;
static StreamPtr g_rel_on_asr;

static void asr_count(StreamPtr s, uint16_t ev, MacPtr32 user, uint16_t reason, MacPtr32 icmp) {
    (void)reason;
    g_asr_stream = s;
    g_asr_user = user;
    if (ev == UDPDataArrival) g_udp_asr++;
    if (ev == UDPICMPReceived) {
        const ICMPReport *r = (const ICMPReport *)pbf_p(icmp);
        g_icmp_asr++;
        if (r) memcpy(&g_rep, r, sizeof g_rep);
    }
}

static void asr_release(StreamPtr s, uint16_t ev, MacPtr32 user, uint16_t reason, MacPtr32 icmp) {
    (void)user; (void)reason; (void)icmp;
    if (ev != UDPDataArrival) return;
    g_udp_asr++;
    g_asr_reentry = (int)ucall(g_x, upb(UDPRelease, s));
}

static void compl_release(MtAnyPb *pb) {
    if (pb->hdr.csCode == UDPRead) g_asr_reentry = (int)ucall(g_x, upb(UDPRelease, g_rel_on_asr));
}

static UDPiopb *uread(pbt *x, StreamPtr s, uint16_t timeout_s) {
    UDPiopb *pb = upb(UDPRead, s);
    pb->csParam.receive.timeOut = timeout_s;
    ucall(x, pb);
    return pb;
}

/* A write of one datagram (one WDS entry) to the peer. */
static UDPiopb *uwrite(pbt *x, StreamPtr s, const void *data, uint16_t len, uint8_t cksum) {
    UDPiopb *pb = upb(UDPWrite, s);
    wdsEntry *w = (wdsEntry *)pbf_alloc(2 * sizeof(wdsEntry));
    uint8_t *b = (uint8_t *)pbf_alloc(len ? len : 1);
    wdsEntry e[2];
    memcpy(b, data, len);
    e[0].length = len; e[0].ptr = len ? pbf_a(b) : 0;
    e[1].length = 0; e[1].ptr = 0;
    memcpy(w, e, sizeof e);
    pb->csParam.send.remoteHost = T_PEER;
    pb->csParam.send.remotePort = PEERPORT;
    pb->csParam.send.wdsPtr = pbf_a(w);
    pb->csParam.send.checkSum = cksum;
    ucall(x, pb);
    return pb;
}

static void create_min_buffer_and_port_rules(void) {
    static const uint32_t bad[] = {0, 1, 1023, 1024, 2047};
    pbt x;
    UDPiopb *pb;
    uint8_t *buf;
    unsigned i;
    uint16_t port = 0;
    pbt_init(&x, 1);
    buf = (uint8_t *)pbf_alloc(8192);
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        pb = upb(UDPCreate, 0);
        pb->csParam.create.rcvBuff = pbf_a(buf);
        pb->csParam.create.rcvBuffLen = bad[i];
        T_EQ(ucall(&x, pb), invalidLength);               /* matrix 900: 2047 and below */
        T_EQ(pb->udpStream, 0);
    }
    pb = upb(UDPCreate, 0);                               /* null buffer, long enough */
    pb->csParam.create.rcvBuffLen = 2048;
    T_EQ(ucall(&x, pb), invalidBufPtr);
    T_EQ(ucall(&x, upb(UDPCreate, 0)), invalidLength);    /* matrix 120.20: zeroed PB */
    T_EQ(ucall(&x, upb(UDPMultiCreate, 0)), invalidLength);   /* matrix 120.27, 911.27 */
    T_CHECK(pbt_ucreate(&x, 7777, 2048, 0, 0, &port) != 0);   /* matrix 900: 2048 works */
    T_EQ(port, 7777);
    T_EQ(pbt_ucreate(&x, 7777, 4096, 0, 0, NULL), 0);     /* matrix 901b: same port twice */
    pb = upb(UDPCreate, 0);
    pb->csParam.create.rcvBuff = pbf_a(buf);
    pb->csParam.create.rcvBuffLen = 4096;
    pb->csParam.create.localPort = 7777;
    T_EQ(ucall(&x, pb), duplicateSocket);                 /* matrix 901b: -23017 */
    pb = upb(UDPCreate, 0);                               /* matrix 901c: localPort + endingPort */
    pb->csParam.create.rcvBuff = pbf_a(buf);
    pb->csParam.create.rcvBuffLen = 4096;
    pb->csParam.create.localPort = 7780;
    pb->csParam.create.endingPort = 7790;
    T_EQ(ucall(&x, pb), 0);
    T_EQ(pb->csParam.create.localPort, 7780);
    {   /* the table: 8 streams, then insufficientResources; a freed slot comes back */
        StreamPtr all[16];
        int n = 0, k;
        for (k = 0; k < 16; k++) {
            all[k] = pbt_ucreate(&x, 0, 2048, 0, 0, NULL);
            n += all[k] != 0;
        }
        T_EQ(n, 8 - 2);                                   /* 7777 and 7780 hold two */
        T_EQ(pbf_count('A'), 0);
    }
    T_EQ(pbf_bad, 0);
    T_EQ(pbf_irq, 0);
}

static void create_with_port0_assigns(void) {
    pbt x;
    uint16_t p1 = 0, p2 = 0;
    StreamPtr s1, s2;
    pbt_init(&x, 1);
    s1 = pbt_ucreate(&x, 0, 2048, 0, 0, &p1);
    s2 = pbt_ucreate(&x, 0, 2048, 0, 0, &p2);
    T_CHECK(s1 != 0 && s2 != 0 && s1 != s2);
    T_CHECK(p1 != 0 && p2 != 0 && p1 != p2);              /* matrix 901: two ephemeral ports, written back */
    T_EQ(ucall(&x, upb(UDPRelease, s1)), 0);
    {   /* the assigned port is the one on the wire */
        pbt_udpv v;
        fake_clear(&x.fl);
        uwrite(&x, s2, "hi", 2, 1);
        T_CHECK(pbt_last_udp(&x, &v));
        T_EQ(v.sport, p2);
    }
}

static void write_read_roundtrip(void) {
    pbt x;
    StreamPtr s;
    uint16_t port = 0;
    UDPiopb *w, *r, *b;
    pbt_udpv v;
    uint8_t *got;
    MacPtr32 buf;
    pbt_init(&x, 1);
    g_udp_asr = 0;
    s = pbt_ucreate(&x, 0, 2048, pbf_asr(asr_count), 0x77, &port);
    fake_clear(&x.fl);
    w = uwrite(&x, s, "ping-9", 6, 1);                   /* matrix 903: write 0 */
    T_EQ(w->ioResult, 0);
    T_CHECK(pbt_last_udp(&x, &v));
    T_EQ(v.dport, PEERPORT);
    T_EQ(v.len, 6);
    T_MEMEQ(v.data, "ping-9", 6);
    T_CHECK(v.cksum != 0 && v.cksum_ok);
    T_EQ(v.dst, T_PEER);
    r = uread(&x, s, 5);                                  /* matrix 903: the read is pending */
    T_EQ(r->ioResult, inProgress);
    pbf_nlog = 0;
    pbt_udp_in(&x, PEERPORT, port, "pong-9!!!", 9);       /* the echo */
    T_EQ(r->ioResult, 0);                                 /* completed inside mt_input */
    T_EQ(r->csParam.receive.rcvBuffLen, 9);               /* matrix 903: 9 bytes */
    T_EQ(r->csParam.receive.remoteHost, T_PEER);
    T_EQ(r->csParam.receive.remotePort, PEERPORT);
    buf = r->csParam.receive.rcvBuff;                     /* a pointer inside the stream buffer */
    got = (uint8_t *)pbf_p(buf);
    T_CHECK(got != NULL);
    if (got) T_MEMEQ(got, "pong-9!!!", 9);
    T_EQ(pbf_count('C'), 1);
    T_EQ(pbf_count('A'), 0);                              /* matrix 903: no UDPDataArrival with a read pending */
    b = upb(UDPBfrReturn, s);
    b->csParam.receive.rcvBuff = buf;
    T_EQ(ucall(&x, b), 0);                                /* matrix 903 */
    T_EQ(pbf_bad, 0);
}

static void data_arrival_asr_and_read_at_once(void) {
    pbt x;
    StreamPtr s;
    uint16_t port = 0;
    UDPiopb *r;
    pbt_init(&x, 1);
    g_udp_asr = 0;
    s = pbt_ucreate(&x, 0, 2048, pbf_asr(asr_count), 0x4242, &port);
    pbt_udp_in(&x, PEERPORT, port, "early", 5);           /* matrix 903b: no read pending */
    T_EQ(g_udp_asr, 1);
    T_EQ(g_asr_stream, s);
    T_EQ(g_asr_user, 0x4242);                             /* the userDataPtr given at create */
    pbf_nlog = 0;
    r = uread(&x, s, 5);
    T_EQ(r->ioResult, 0);                                 /* matrix 903b: returns it at once */
    T_EQ(r->csParam.receive.rcvBuffLen, 5);
    T_EQ(pbf_count('C'), 1);                              /* (completed once, by the driver entry) */
    pbt_udp_in(&x, PEERPORT, port, "two", 3);             /* queued behind the lent datagram */
    T_EQ(g_udp_asr, 2);
    T_EQ(pbf_bad, 0);
}

static void read_timeout_completes_with_matrix_error(void) {
    pbt x;
    StreamPtr s;
    UDPiopb *r, *r0;
    pbt_init(&x, 1);
    s = pbt_ucreate(&x, 0, 2048, 0, 0, NULL);
    r = uread(&x, s, 2);
    r0 = uread(&x, s, 0);                                 /* matrix 906: pending (0 = no timeout) */
    T_EQ(r->ioResult, inProgress);
    T_EQ(r0->ioResult, inProgress);
    T_EQ(mt_pb_next_deadline_ms(x.d), 2000);
    pbf_nlog = 0;
    pbf_now += 1999;
    mt_pb_tick(x.d);
    T_EQ(r->ioResult, inProgress);
    pbf_now += 1;
    T_EQ(mt_pb_next_deadline_ms(x.d), 0);
    mt_pb_tick(x.d);
    T_EQ(r->ioResult, commandTimeout);                    /* matrix 905: -23016 after timeOut seconds */
    T_EQ(r0->ioResult, inProgress);
    T_EQ(pbf_count('C'), 1);
    T_EQ(mt_pb_udp_deadline(x.d), MT_NO_DEADLINE);        /* the untimed read arms nothing */
    pbf_now += 100000;
    mt_pb_tick(x.d);
    T_EQ(r0->ioResult, inProgress);
}

static void release_completes_pending_read(void) {
    pbt x;
    StreamPtr s, dead;
    UDPiopb *r1, *r2, *rel;
    pbt_init(&x, 1);
    s = pbt_ucreate(&x, 0, 2048, pbf_asr(asr_count), 0, NULL);
    dead = s;
    r1 = uread(&x, s, 0);
    r2 = uread(&x, s, 0);
    T_EQ(r1->ioResult, inProgress);                       /* matrix 906: both accepted */
    T_EQ(r2->ioResult, inProgress);
    rel = upb(UDPRelease, s);
    pbf_nlog = 0;
    T_EQ(ucall(&x, rel), 0);                              /* matrix 906: release 0 */
    T_EQ(r1->ioResult, connectionTerminated);             /* ... both reads complete -23012 */
    T_EQ(r2->ioResult, connectionTerminated);
    T_EQ(pbf_count('C'), 3);                              /* two reads + the release, once each */
    T_CHECK(pbf_find('C', pbf_a(r1), 0) < pbf_find('C', pbf_a(rel), 0));   /* reads before the release */
    T_EQ(pbf_count('A'), 0);
    T_CHECK(rel->csParam.create.rcvBuff != 0);            /* the buffer goes back to the app */
    T_EQ(rel->csParam.create.rcvBuffLen, 2048);
    T_EQ(ucall(&x, upb(UDPRelease, dead)), invalidStreamPtr);   /* matrix 902: twice */
    T_EQ(ucall(&x, upb(UDPRelease, 0)), invalidStreamPtr);      /* matrix 902: stream 0 */
    T_EQ(uread(&x, dead, 1)->ioResult, invalidStreamPtr);       /* matrix 907 */
    {   /* a released stream's pending-read timer and late datagrams do nothing */
        uint16_t port = 0;
        StreamPtr s2 = pbt_ucreate(&x, 0, 2048, pbf_asr(asr_count), 0, &port);
        T_CHECK(s2 != 0 && s2 != dead);
        T_EQ(uread(&x, dead, 1)->ioResult, invalidStreamPtr);   /* the old handle stays dead */
    }
    T_EQ(pbf_bad, 0);
}

static void rcv_buffer_return(void) {
    pbt x;
    StreamPtr s;
    uint16_t port = 0;
    UDPiopb *r1, *r2, *b;
    MacPtr32 k1;
    pbt_init(&x, 1);
    s = pbt_ucreate(&x, 0, 2048, 0, 0, &port);
    pbt_udp_in(&x, PEERPORT, port, "first", 5);
    pbt_udp_in(&x, PEERPORT + 1u, port, "second!", 7);
    r1 = uread(&x, s, 0);
    T_EQ(r1->ioResult, 0);
    k1 = r1->csParam.receive.rcvBuff;
    r2 = uread(&x, s, 0);                                 /* matrix 904b-d: no return needed first */
    T_EQ(r2->ioResult, 0);
    T_EQ(r2->csParam.receive.rcvBuffLen, 7);
    b = upb(UDPBfrReturn, s);
    b->csParam.receive.rcvBuff = k1 + 1;                  /* made up */
    T_EQ(ucall(&x, b), invalidBufPtr);                    /* matrix 903: -23013 */
    b = upb(UDPBfrReturn, s);
    b->csParam.receive.rcvBuff = k1;
    T_EQ(ucall(&x, b), 0);
    T_EQ(r2->csParam.receive.remotePort, PEERPORT + 1u);
    b = upb(UDPBfrReturn, s);
    b->csParam.receive.rcvBuff = k1;
    T_EQ(ucall(&x, b), invalidBufPtr);                    /* matrix 903: again -23013 */
    b = upb(UDPBfrReturn, s);
    b->csParam.receive.rcvBuff = r2->csParam.receive.rcvBuff;
    T_EQ(ucall(&x, b), 0);
    T_EQ(ucall(&x, upb(UDPBfrReturn, 0)), invalidStreamPtr);   /* matrix 120.22 */
    {   /* the buffer is free again: a full-size run of datagrams fits */
        int i, n = 0;
        uint8_t big[400];
        memset(big, 7, sizeof big);
        for (i = 0; i < 10; i++) {
            UDPiopb *r;
            pbt_udp_in(&x, PEERPORT, port, big, sizeof big);
            r = uread(&x, s, 0);
            if (r->ioResult != 0) break;
            n++;
            b = upb(UDPBfrReturn, s);
            b->csParam.receive.rcvBuff = r->csParam.receive.rcvBuff;
            T_EQ(ucall(&x, b), 0);
        }
        T_EQ(n, 10);
    }
    T_EQ(pbf_bad, 0);
}

static void write_variants(void) {
    pbt x;
    StreamPtr s;
    UDPiopb *w;
    pbt_udpv v;
    wdsEntry *wds;
    uint8_t *odd, *big;
    wdsEntry e[4];
    int i;
    pbt_init(&x, 1);
    s = pbt_ucreate(&x, 0, 2048, 0, 0, NULL);
    fake_clear(&x.fl);
    w = uwrite(&x, s, "", 0, 1);                          /* matrix 904: zero-length datagram */
    T_EQ(w->ioResult, 0);
    T_CHECK(pbt_last_udp(&x, &v));
    T_EQ(v.len, 0);
    T_CHECK(v.cksum_ok);
    fake_clear(&x.fl);
    w = uwrite(&x, s, "nochecksum", 10, 0);               /* matrix 904d: checkSum 0 */
    T_EQ(w->ioResult, 0);
    T_CHECK(pbt_last_udp(&x, &v));
    T_EQ(v.cksum, 0);
    T_EQ(v.len, 10);
    odd = (uint8_t *)pbf_alloc(64) + 1;                   /* matrix 904b: odd data address */
    memcpy(odd, "odd-addr!", 9);
    wds = (wdsEntry *)pbf_alloc(4 * sizeof(wdsEntry));
    e[0].length = 9; e[0].ptr = pbf_a(odd);
    e[1].length = 0; e[1].ptr = 0;
    memcpy(wds, e, 2 * sizeof e[0]);
    w = upb(UDPWrite, s);
    w->csParam.send.remoteHost = T_PEER;
    w->csParam.send.remotePort = PEERPORT;
    w->csParam.send.wdsPtr = pbf_a(wds);
    w->csParam.send.checkSum = 1;
    fake_clear(&x.fl);
    T_EQ(ucall(&x, w), 0);
    T_CHECK(pbt_last_udp(&x, &v));
    T_EQ(v.len, 9);
    T_MEMEQ(v.data, "odd-addr!", 9);
    {   /* matrix 904c: two entries are gathered in order */
        uint8_t *a = (uint8_t *)pbf_alloc(8), *b2 = (uint8_t *)pbf_alloc(8);
        memcpy(a, "abc", 3);
        memcpy(b2, "defgh", 5);
        e[0].length = 3; e[0].ptr = pbf_a(a);
        e[1].length = 5; e[1].ptr = pbf_a(b2);
        e[2].length = 0; e[2].ptr = 0;
        memcpy(wds, e, 3 * sizeof e[0]);
        fake_clear(&x.fl);
        w = upb(UDPWrite, s);
        w->csParam.send.remoteHost = T_PEER;
        w->csParam.send.remotePort = PEERPORT;
        w->csParam.send.wdsPtr = pbf_a(wds);
        w->csParam.send.checkSum = 1;
        T_EQ(ucall(&x, w), 0);
        T_CHECK(pbt_last_udp(&x, &v));
        T_EQ(v.len, 8);
        T_MEMEQ(v.data, "abcdefgh", 8);
        T_CHECK(v.cksum_ok);
    }
    w = upb(UDPWrite, s);                                 /* null wdsPtr */
    w->csParam.send.remoteHost = T_PEER;
    w->csParam.send.remotePort = PEERPORT;
    T_EQ(ucall(&x, w), invalidWDS);                       /* matrix 911.28 (UDPMultiSend), 990 */
    {   /* one huge entry goes out fragmented; a multi-entry total over one frame is refused */
        big = (uint8_t *)pbf_alloc(4000);
        for (i = 0; i < 4000; i++) big[i] = (uint8_t)i;
        e[0].length = 3000; e[0].ptr = pbf_a(big);        /* matrix 904g: 3000 bytes, write 0 */
        e[1].length = 0; e[1].ptr = 0;
        memcpy(wds, e, 2 * sizeof e[0]);
        w = upb(UDPWrite, s);
        w->csParam.send.remoteHost = T_PEER;
        w->csParam.send.remotePort = PEERPORT;
        w->csParam.send.wdsPtr = pbf_a(wds);
        w->csParam.send.checkSum = 1;
        fake_clear(&x.fl);
        T_EQ(ucall(&x, w), 0);
        T_CHECK(x.fl.n >= 3);                             /* 3008 bytes: three frames */
        e[0].length = 1000; e[0].ptr = pbf_a(big);
        e[1].length = 1000; e[1].ptr = pbf_a(big + 1000);
        e[2].length = 0; e[2].ptr = 0;
        memcpy(wds, e, 3 * sizeof e[0]);
        T_EQ(ucall(&x, w), invalidLength);                /* gather limit: 1472 */
        e[0].length = 700; e[1].length = 700;
        memcpy(wds, e, 3 * sizeof e[0]);
        T_EQ(ucall(&x, w), 0);
    }
    {   /* unterminated WDS (every entry non-empty) */
        wdsEntry *many = (wdsEntry *)pbf_alloc(MT_PB_WDS_MAX * sizeof(wdsEntry) + 16);
        wdsEntry one;
        one.length = 1; one.ptr = pbf_a(big);
        for (i = 0; i < MT_PB_WDS_MAX + 2; i++) memcpy(many + i, &one, sizeof one);
        w = upb(UDPWrite, s);
        w->csParam.send.remoteHost = T_PEER;
        w->csParam.send.remotePort = PEERPORT;
        w->csParam.send.wdsPtr = pbf_a(many);
        T_EQ(ucall(&x, w), invalidWDS);
        e[0].length = 5; e[0].ptr = 0;                    /* length without a pointer */
        memcpy(wds, e, sizeof e[0]);
        w = upb(UDPWrite, s);
        w->csParam.send.remoteHost = T_PEER;
        w->csParam.send.remotePort = PEERPORT;
        w->csParam.send.wdsPtr = pbf_a(wds);
        T_EQ(ucall(&x, w), invalidWDS);
    }
    T_EQ(ucall(&x, upb(UDPWrite, 0)), invalidStreamPtr);  /* matrix 120.23 */
    T_EQ(pbf_bad, 0);
}

static void mtu_call(void) {
    pbt x;
    UDPiopb *pb;
    pbt_init(&x, 1);
    pb = upb(UDPMaxMTUSize, 0);                           /* matrix 910: no stream needed */
    T_EQ(ucall(&x, pb), 0);
    T_EQ(pb->csParam.mtu.mtuSize, 548);                   /* remoteHost 0: off our subnet */
    pb = upb(UDPMaxMTUSize, 0);
    pb->csParam.mtu.remoteHost = 0x08080808u;
    T_EQ(ucall(&x, pb), 0);
    T_EQ(pb->csParam.mtu.mtuSize, 548);
    pb = upb(UDPMaxMTUSize, 0);
    pb->csParam.mtu.remoteHost = T_PEER;                  /* on-link: the whole frame */
    T_EQ(ucall(&x, pb), 0);
    T_EQ(pb->csParam.mtu.mtuSize, 1472);
}

static void stream_check_sweep(void) {
    pbt x;
    StreamPtr s, ts;
    int16_t cs;
    pbt_init(&x, 1);
    s = pbt_ucreate(&x, 0, 2048, 0, 0, NULL);
    ts = pbt_create(&x, 4096, 0, 0);
    ucall(&x, upb(UDPRelease, s));
    for (cs = UDPRead; cs <= UDPMultiRead; cs++) {
        int16_t want = cs == UDPMaxMTUSize || cs == UDPStatus ? 0
                     : cs == UDPMultiCreate ? invalidLength : invalidStreamPtr;
        T_EQ(ucall(&x, upb(cs, 0)), want);                /* matrix 120.20-29: stream 0 */
        T_EQ(ucall(&x, upb(cs, s)), want);                /* matrix 121.20-29: released stream */
        if (want == invalidStreamPtr) T_EQ(ucall(&x, upb(cs, ts)), invalidStreamPtr);  /* a TCP handle */
    }
    T_EQ(ucall(&x, upb(UDPCreate, 0)), invalidLength);    /* matrix 120.20 */
    T_EQ(pbt_simple(&x, TCPStatus, s), invalidStreamPtr); /* and a UDP handle is no TCP stream */
    T_EQ(pbt_simple(&x, TCPRelease, s), invalidStreamPtr);
    T_EQ(pbf_bad, 0);
}

static void icmp_report_asr(void) {
    pbt x;
    StreamPtr s;
    uint16_t port = 0;
    uint8_t m[36], f[120];
    uint32_t sum = 0;
    uint16_t n, i;
    pbt_init(&x, 1);
    g_icmp_asr = 0;
    s = pbt_ucreate(&x, 0, 2048, pbf_asr(asr_count), 0, &port);
    memset(m, 0, sizeof m);
    m[0] = 3;                                             /* destination unreachable, port */
    m[1] = 3;
    m[8] = 0x45;                                          /* quoted header: our datagram */
    m[8 + 9] = 17;
    mt_wr32(m + 8 + 12, T_ME);
    mt_wr32(m + 8 + 16, T_PEER);
    mt_wr16(m + 28, port);
    mt_wr16(m + 30, PEERPORT);
    for (i = 0; i < sizeof m; i += 2) sum += (uint32_t)(m[i] << 8 | m[i + 1]);
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    mt_wr16(m + 2, (uint16_t)~sum);
    n = mk_ip(f, T_PEER, T_ME, 1, 77, 0, m, sizeof m);
    mt_pb_input(x.d, f, n);
    T_EQ(g_icmp_asr, 1);                                  /* UDPICMPReceived (obs: never seen in Snow) */
    T_EQ(g_rep.streamPtr, s);
    T_EQ(g_rep.reportType, portUnreach);
    T_EQ(g_rep.localPort, port);
    T_EQ(g_rep.localHost, T_ME);
    T_EQ(uread(&x, s, 0)->ioResult, inProgress);          /* the stream lives on */
}

static void release_inside_callbacks(void) {
    pbt x;
    StreamPtr s;
    uint16_t port = 0;
    UDPiopb *r;
    pbt_init(&x, 1);
    g_x = &x;
    g_udp_asr = 0;
    g_asr_reentry = 99;
    s = pbt_ucreate(&x, 0, 2048, pbf_asr(asr_release), 0, &port);
    pbt_udp_in(&x, PEERPORT, port, "x", 1);               /* ASR releases the stream it is running for */
    T_EQ(g_udp_asr, 1);
    T_EQ(g_asr_reentry, 0);
    T_EQ(uread(&x, s, 0)->ioResult, invalidStreamPtr);
    pbt_udp_in(&x, PEERPORT, port, "y", 1);               /* nobody listens: no callback, no crash */
    T_EQ(g_udp_asr, 1);
    /* a read completion routine that releases its stream: the datagram that completed the read
     * is not delivered twice, the stream is gone afterwards */
    s = pbt_ucreate(&x, 0, 2048, pbf_asr(asr_count), 0, &port);
    g_rel_on_asr = s;
    g_asr_reentry = 99;
    r = upb(UDPRead, s);
    r->ioCompletion = pbf_compl(compl_release);
    ucall(&x, r);
    T_EQ(r->ioResult, inProgress);
    pbt_udp_in(&x, PEERPORT, port, "z", 1);
    T_EQ(r->ioResult, 0);
    T_EQ(g_asr_reentry, 0);                               /* the release inside the completion */
    T_EQ(uread(&x, s, 0)->ioResult, invalidStreamPtr);
    pbt_udp_in(&x, PEERPORT, port, "w", 1);
    T_EQ(pbf_count('A'), 1);                              /* only the asr_release one above */
    T_EQ(pbf_bad, 0);
    T_EQ(pbf_irq, 0);
}

/* Every UDP csCode with its PB in an exact-size (58 byte) heap block. */
static int16_t uexact(pbt *x, const UDPiopb *pb) {
    void *h = malloc(sizeof(UDPiopb));
    int16_t r;
    memcpy(h, pb, sizeof(UDPiopb));
    r = mt_pb_control(x->d, (MtAnyPb *)h);
    memcpy((void *)pb, h, sizeof(UDPiopb));               /* outputs back to the fake-memory copy */
    free(h);
    return r;
}

static void exact_size_pbs(void) {
    pbt x;
    StreamPtr s;
    uint16_t port = 0;
    UDPiopb *pb;
    uint8_t *buf;
    int16_t cs;
    T_EQ(sizeof(UDPiopb), 58);
    pbt_init(&x, 1);
    buf = (uint8_t *)pbf_alloc(2048);
    pb = upb(UDPCreate, 0);
    pb->csParam.create.rcvBuff = pbf_a(buf);
    pb->csParam.create.rcvBuffLen = 2048;
    T_EQ(uexact(&x, pb), 0);
    s = pb->udpStream;
    port = pb->csParam.create.localPort;
    T_CHECK(s != 0);
    pbt_udp_in(&x, PEERPORT, port, "abc", 3);
    pb = upb(UDPRead, s);
    T_EQ(uexact(&x, pb), 0);
    pbf_nlog = 0;
    {
        UDPiopb *b = upb(UDPBfrReturn, s);
        b->csParam.receive.rcvBuff = pb->csParam.receive.rcvBuff;
        T_EQ(uexact(&x, b), 0);
    }
    {   /* UDPWrite with a one-entry WDS */
        wdsEntry *w = (wdsEntry *)pbf_alloc(2 * sizeof(wdsEntry)), e[2];
        e[0].length = 3; e[0].ptr = pbf_a(buf);
        e[1].length = 0; e[1].ptr = 0;
        memcpy(w, e, sizeof e);
        pb = upb(UDPWrite, s);
        pb->csParam.send.remoteHost = T_PEER;
        pb->csParam.send.remotePort = PEERPORT;
        pb->csParam.send.wdsPtr = pbf_a(w);
        T_EQ(uexact(&x, pb), 0);
    }
    T_EQ(uexact(&x, upb(UDPMaxMTUSize, 0)), 0);
    T_EQ(uexact(&x, upb(UDPStatus, 0)), 0);
    for (cs = UDPMultiCreate; cs <= UDPMultiRead; cs++) uexact(&x, upb(cs, s));
    uexact(&x, upb(UDPRead, 0));
    pb = upb(UDPRelease, s);
    T_EQ(uexact(&x, pb), 0);
    T_EQ(pb->csParam.create.rcvBuffLen, 2048);
    T_EQ(pbf_bad, 0);
}

static int16_t ureturn(pbt *x, StreamPtr s, MacPtr32 p) {
    UDPiopb *b = upb(UDPBfrReturn, s);
    b->csParam.receive.rcvBuff = p;
    return ucall(x, b);
}

/* Matrix 904-904d: MacTCP returns each datagram although the earlier ones were never given back
 * with UDPBfrReturn. Each read gets its own datagram, in arrival order, in place. */
static void udp_two_reads_without_return(void) {
    pbt x;
    StreamPtr s;
    uint16_t port = 0;
    UDPiopb *r1, *r2, *r3;
    pbt_init(&x, 1);
    s = pbt_ucreate(&x, 0, 2048, 0, 0, &port);
    pbt_udp_in(&x, PEERPORT, port, "", 0);                /* matrix 904: the empty datagram */
    r1 = uread(&x, s, 2);
    T_EQ(r1->ioResult, 0);
    T_EQ(r1->csParam.receive.rcvBuffLen, 0);
    r2 = uread(&x, s, 3);                                 /* matrix 904b: no return in between */
    T_EQ(r2->ioResult, inProgress);
    pbt_udp_in(&x, PEERPORT, port, "odd-addr!", 9);
    T_EQ(r2->ioResult, 0);                                /* matrix 904b: rd 0, n 9 */
    T_EQ(r2->csParam.receive.rcvBuffLen, 9);
    T_MEMEQ(pbf_p(r2->csParam.receive.rcvBuff), "odd-addr!", 9);
    pbt_udp_in(&x, PEERPORT, port, "multi", 5);           /* matrix 904c: arrives before the read */
    r3 = uread(&x, s, 3);
    T_EQ(r3->ioResult, 0);                                /* at once (903b) */
    T_EQ(r3->csParam.receive.rcvBuffLen, 5);
    T_MEMEQ(pbf_p(r3->csParam.receive.rcvBuff), "multi", 5);
    T_MEMEQ(pbf_p(r2->csParam.receive.rcvBuff), "odd-addr!", 9);   /* the earlier loan is intact */
    T_CHECK(r2->csParam.receive.rcvBuff != r3->csParam.receive.rcvBuff);
    T_EQ(ureturn(&x, s, r1->csParam.receive.rcvBuff), 0);
    T_EQ(ureturn(&x, s, r2->csParam.receive.rcvBuff), 0);
    T_EQ(ureturn(&x, s, r3->csParam.receive.rcvBuff), 0);
    T_EQ(ureturn(&x, s, r3->csParam.receive.rcvBuff), invalidBufPtr);   /* matrix 903: again */
    T_EQ(pbf_bad, 0);
}

static void udp_return_out_of_order(void) {
    pbt x;
    StreamPtr s;
    uint16_t port = 0;
    UDPiopb *r[3], *w;
    pbt_init(&x, 1);
    s = pbt_ucreate(&x, 0, 2048, 0, 0, &port);
    pbt_udp_in(&x, PEERPORT, port, "aaaa", 4);
    pbt_udp_in(&x, PEERPORT + 1u, port, "bbbbbbb", 7);
    pbt_udp_in(&x, PEERPORT + 2u, port, "c", 1);
    r[0] = uread(&x, s, 0); r[1] = uread(&x, s, 0); r[2] = uread(&x, s, 0);
    T_EQ(r[0]->ioResult, 0); T_EQ(r[1]->ioResult, 0); T_EQ(r[2]->ioResult, 0);
    T_EQ(r[1]->csParam.receive.remotePort, PEERPORT + 1u);
    T_EQ(r[2]->csParam.receive.remotePort, PEERPORT + 2u);
    T_EQ(ureturn(&x, s, r[1]->csParam.receive.rcvBuff + 1), invalidBufPtr);   /* made up */
    T_EQ(ureturn(&x, s, r[1]->csParam.receive.rcvBuff), 0);                   /* the middle one */
    T_EQ(ureturn(&x, s, r[1]->csParam.receive.rcvBuff), invalidBufPtr);       /* already back */
    T_MEMEQ(pbf_p(r[0]->csParam.receive.rcvBuff), "aaaa", 4);
    T_MEMEQ(pbf_p(r[2]->csParam.receive.rcvBuff), "c", 1);
    pbt_udp_in(&x, PEERPORT + 3u, port, "dd", 2);
    w = uread(&x, s, 0);                                  /* the next datagram, not a returned one */
    T_EQ(w->ioResult, 0);
    T_EQ(w->csParam.receive.remotePort, PEERPORT + 3u);
    T_MEMEQ(pbf_p(w->csParam.receive.rcvBuff), "dd", 2);
    T_EQ(ureturn(&x, s, r[2]->csParam.receive.rcvBuff), 0);
    T_EQ(ureturn(&x, s, r[0]->csParam.receive.rcvBuff), 0);
    T_EQ(ureturn(&x, s, w->csParam.receive.rcvBuff), 0);
    T_EQ(ureturn(&x, s, r[0]->csParam.receive.rcvBuff), invalidBufPtr);
    {   /* every loan is back: the whole buffer is free again */
        int i, n = 0;
        uint8_t big[400];
        memset(big, 7, sizeof big);
        for (i = 0; i < 5; i++) pbt_udp_in(&x, PEERPORT, port, big, sizeof big);
        for (i = 0; i < 5; i++) n += uread(&x, s, 0)->ioResult == 0;
        T_EQ(n, 5);
    }
    T_EQ(pbf_bad, 0);
}

/* MT_PB_UDPLOANS datagrams can be held per stream. A further read gets what MacTCP gives a read for
 * which no datagram can be handed out (its buffer full of unreturned datagrams): it waits and ends
 * with commandTimeout -23016 (matrix 905); a return in time serves it. */
static void udp_loan_table_full_returns_error(void) {
    pbt x;
    StreamPtr s;
    uint16_t port = 0;
    UDPiopb *r[MT_PB_UDPLOANS], *w, *w2;
    int i;
    pbt_init(&x, 1);
    s = pbt_ucreate(&x, 0, 4096, 0, 0, &port);
    for (i = 0; i < MT_PB_UDPLOANS + 2; i++) pbt_udp_in(&x, PEERPORT + (uint16_t)i, port, "dg", 2);
    for (i = 0; i < MT_PB_UDPLOANS; i++) {
        r[i] = uread(&x, s, 0);
        T_EQ(r[i]->ioResult, 0);
        T_EQ(r[i]->csParam.receive.remotePort, PEERPORT + (uint16_t)i);
    }
    w = uread(&x, s, 2);                                  /* the table is full */
    T_EQ(w->ioResult, inProgress);
    pbf_now += 2000;
    mt_pb_tick(x.d);
    T_EQ(w->ioResult, commandTimeout);                    /* matrix 905: -23016 */
    w = uread(&x, s, 2);
    w2 = uread(&x, s, 2);
    T_EQ(w->ioResult, inProgress);
    T_EQ(ureturn(&x, s, r[3]->csParam.receive.rcvBuff), 0);    /* one back: the oldest read is served */
    T_EQ(w->ioResult, 0);
    T_EQ(w->csParam.receive.remotePort, PEERPORT + MT_PB_UDPLOANS);
    T_EQ(w2->ioResult, inProgress);
    T_EQ(ureturn(&x, s, r[0]->csParam.receive.rcvBuff), 0);
    T_EQ(w2->ioResult, 0);
    T_EQ(w2->csParam.receive.remotePort, PEERPORT + MT_PB_UDPLOANS + 1u);
    T_EQ(pbf_bad, 0);
}

/* UDPRelease (and the goodbye kiss when the app quits) with datagrams still lent: the buffer goes
 * back to the app at once, nothing writes into it afterwards, the loans are forgotten. */
static void udp_release_with_loans_outstanding(void) {
    pbt x;
    StreamPtr s;
    uint16_t port = 0;
    UDPiopb *r1, *r2, *rel;
    uint8_t *buf, snap[2048];
    int pass;
    pbt_init(&x, 1);
    for (pass = 0; pass < 2; pass++) {
        s = pbt_ucreate(&x, 0, 2048, pbf_asr(asr_count), 0, &port);
        pbt_udp_in(&x, PEERPORT, port, "one", 3);
        pbt_udp_in(&x, PEERPORT, port, "two", 3);
        pbt_udp_in(&x, PEERPORT, port, "three", 5);
        r1 = uread(&x, s, 0);
        r2 = uread(&x, s, 0);
        T_EQ(r1->ioResult, 0); T_EQ(r2->ioResult, 0);
        T_EQ(ureturn(&x, s, r2->csParam.receive.rcvBuff), 0);   /* one returned, r1 still held */
        if (pass == 0) {
            rel = upb(UDPRelease, s);
            T_EQ(ucall(&x, rel), 0);
            T_EQ(rel->csParam.create.rcvBuffLen, 2048);
            buf = (uint8_t *)pbf_p(rel->csParam.create.rcvBuff);
        } else {
            buf = (uint8_t *)pbf_p(r1->csParam.receive.rcvBuff) - 8;   /* the first record */
            mt_pb_goodbye(x.d);                           /* the app quits */
        }
        T_CHECK(buf != NULL);
        if (!buf) return;
        memcpy(snap, buf, sizeof snap);
        T_EQ(ureturn(&x, s, r1->csParam.receive.rcvBuff), invalidStreamPtr);   /* matrix 120.22 */
        T_EQ(uread(&x, s, 0)->ioResult, invalidStreamPtr);
        pbt_udp_in(&x, PEERPORT, port, "late", 4);        /* the port is closed */
        pbf_now += 5000;
        mt_pb_tick(x.d);
        T_MEMEQ(buf, snap, sizeof snap);                  /* not written after the release */
        T_MEMEQ(pbf_p(r1->csParam.receive.rcvBuff), "one", 3);
        {   /* a new stream in the same slot starts without loans */
            uint16_t p2 = 0;
            StreamPtr s2 = pbt_ucreate(&x, 0, 2048, 0, 0, &p2);
            UDPiopb *n;
            T_CHECK(s2 != 0);
            pbt_udp_in(&x, PEERPORT, p2, "new", 3);
            n = uread(&x, s2, 0);
            T_EQ(n->ioResult, 0);
            T_EQ(ureturn(&x, s2, r1->csParam.receive.rcvBuff), invalidBufPtr);
            T_EQ(ureturn(&x, s2, n->csParam.receive.rcvBuff), 0);
            T_EQ(ucall(&x, upb(UDPRelease, s2)), 0);
        }
    }
    T_EQ(pbf_bad, 0);
}

T_MAIN(create_min_buffer_and_port_rules, create_with_port0_assigns, write_read_roundtrip,
       data_arrival_asr_and_read_at_once, read_timeout_completes_with_matrix_error,
       release_completes_pending_read, rcv_buffer_return, write_variants, mtu_call,
       stream_check_sweep, icmp_report_asr, release_inside_callbacks, exact_size_pbs,
       udp_two_reads_without_return, udp_return_out_of_order, udp_loan_table_full_returns_error,
       udp_release_with_loans_outstanding)
