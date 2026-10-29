/* PB dispatcher, TCP calls (part 1): create/release, open, send, receive, no-copy receive.
 * Expected values are real MacTCP 2.1 results; each assertion that encodes one cites its case
 * in docs/dev/MACTCP-REFERENCE-MATRIX.md (raw lines in docs/dev/probe/mactcp-configured.out). */
#include "t.h"
#include "pbfake.h"
#include "mt_util.h"

static void asr_nop(StreamPtr s, uint16_t ev, MacPtr32 user, uint16_t reason, MacPtr32 icmp) {
    (void)s; (void)ev; (void)user; (void)reason; (void)icmp;
}

static void create_min_buffer_rejected(void) {
    static const uint32_t bad[] = {0, 1, 512, 1024, 2048, 4095};
    pbt x;
    TCPiopb *pb;
    uint8_t *buf;
    unsigned i;
    pbt_init(&x, 1);
    buf = (uint8_t *)pbf_alloc(16384);
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        pb = pbt_pb(TCPCreate, 0);
        pb->csParam.create.rcvBuff = pbf_a(buf);
        pb->csParam.create.rcvBuffLen = bad[i];
        T_EQ(pbt_call(&x, pb), invalidLength);           /* matrix 200.0 .. 200.4095 */
        T_EQ(pb->ioResult, invalidLength);
        T_EQ(pb->tcpStream, 0);
    }
    pb = pbt_pb(TCPCreate, 0);                            /* null buffer, length 4096 */
    pb->csParam.create.rcvBuffLen = 4096;
    T_EQ(pbt_call(&x, pb), invalidBufPtr);                /* matrix 201 */
    pb = pbt_pb(TCPCreate, 0);                            /* zeroed PB: the length is checked first */
    T_EQ(pbt_call(&x, pb), invalidLength);                /* matrix 120.30 */
    pb = pbt_pb(TCPCreate, 0);
    pb->csParam.create.rcvBuff = pbf_a(buf);
    pb->csParam.create.rcvBuffLen = 4096;
    T_EQ(pbt_call(&x, pb), 0);                            /* matrix 200.4096 */
    T_CHECK(pb->tcpStream != 0);
    pb = pbt_pb(TCPCreate, 0);                            /* odd buffer address: we accept it */
    pb->csParam.create.rcvBuff = pbf_a(buf + 8193);
    pb->csParam.create.rcvBuffLen = 8191;
    T_EQ(pbt_call(&x, pb), 0);
    T_EQ(pbf_count('C'), 10);                             /* every call completed exactly once */
    T_EQ(pbf_count('A'), 0);
    T_EQ(pbf_bad, 0);
    T_EQ(pbf_irq, 0);
}

static void create_release_roundtrip(void) {
    pbt x;
    TCPiopb *pb;
    StreamPtr s, s2;
    MacPtr32 buf;
    pbt_init(&x, 1);
    s = pbt_create(&x, 4096, pbf_asr(asr_nop), 0x1234);
    T_CHECK(s != 0);
    T_EQ(pbt_simple(&x, TCPStatus, s), connectionDoesntExist);   /* matrix 602: unconnected */
    pb = pbt_pb(TCPRelease, s);
    T_EQ(pbt_call(&x, pb), 0);                            /* matrix 203 */
    buf = pb->csParam.create.rcvBuff;                     /* the buffer is handed back */
    T_CHECK(buf != 0);
    T_EQ(pb->csParam.create.rcvBuffLen, 4096);
    T_EQ(pbf_count('A'), 0);                              /* idle release: no TCPTerminate (Events) */
    T_EQ(pbt_simple(&x, TCPRelease, s), invalidStreamPtr);       /* matrix 203: second release */
    T_EQ(pbt_simple(&x, TCPRelease, 0), invalidStreamPtr);       /* matrix 203: stream 0 */
    T_EQ(pbt_simple(&x, TCPStatus, s), invalidStreamPtr);        /* matrix 121.40 */
    T_EQ(pbt_simple(&x, TCPRelease, s + 1), invalidStreamPtr);   /* misaligned handle */
    T_EQ(pbt_simple(&x, TCPRelease, 0xFFFFFFFEu), invalidStreamPtr);
    s2 = pbt_create(&x, 4096, 0, 0);                      /* a new stream never revives the old handle */
    T_CHECK(s2 != 0 && s2 != s);
    T_EQ(pbt_simple(&x, TCPStatus, s), invalidStreamPtr);
    T_EQ(pbt_simple(&x, TCPStatus, s2), connectionDoesntExist);
    {   /* every slot can be created and released; each re-creation gets a fresh handle */
        StreamPtr all[MT_PB_MAX_TCP];
        int i, n = 0;
        for (i = 0; i < MT_PB_MAX_TCP; i++) {
            all[i] = pbt_create(&x, 4096, 0, 0);
            if (all[i]) n++;
        }
        T_EQ(n, MT_PB_MAX_TCP - 1);                       /* s2 holds one */
        T_EQ(pbt_create(&x, 4096, 0, 0), 0);              /* table full */
        for (i = 0; i < MT_PB_MAX_TCP; i++)
            if (all[i]) T_EQ(pbt_simple(&x, TCPRelease, all[i]), 0);
        for (i = 0; i < MT_PB_MAX_TCP; i++)
            if (all[i]) T_EQ(pbt_simple(&x, TCPStatus, all[i]), invalidStreamPtr);
    }
    T_EQ(pbf_bad, 0);
}

static void active_open_completes_on_established(void) {
    pbt x;
    TCPiopb *pb, *st;
    StreamPtr s;
    tcpv v;
    int n0;
    pbt_init(&x, 1);
    s = pbt_create(&x, 8192, pbf_asr(asr_nop), 0);
    fake_clear(&x.fl);
    pb = pbt_open(&x, s, 0);
    T_CHECK(pb != NULL);
    if (!pb) return;
    T_EQ(pb->ioResult, inProgress);
    T_CHECK(pbt_last_tcp(&x, &v) && v.flags == TF_SYN);  /* the SYN left before the call returned */
    n0 = pbf_nlog;
    pbt_peer(&x, v.sport, 5000, v.seq + 1, TF_SYN | TF_ACK, 8192, NULL, 0);
    T_EQ(pb->ioResult, 0);                                /* completed inside that input */
    T_EQ(pbf_find('C', pbf_a(pb), n0), n0);
    T_EQ(pb->csParam.open.localHost, T_ME);               /* matrix 300: localHost, ephemeral port */
    T_EQ(pb->csParam.open.localPort, v.sport);
    T_CHECK(v.sport != 0);
    T_EQ(pb->csParam.open.remoteHost, T_PEER);
    T_EQ(pb->csParam.open.remotePort, PBT_PEER_PORT);
    T_EQ(pbf_count('A'), 0);
    st = pbt_pb(TCPStatus, s);
    T_EQ(pbt_call(&x, st), 0);
    T_EQ(st->csParam.status.connectionState, ConnectionStateEstablished);   /* matrix 300: state 8 */
    T_EQ(st->csParam.status.sendMaxSegSize, 1460);        /* MacTCP 536 (matrix 300): beaten */
    pb = pbt_pb(TCPActiveOpen, s);                        /* second open on a connected stream */
    pb->csParam.open.remoteHost = T_PEER;
    pb->csParam.open.remotePort = 80;
    T_EQ(pbt_call(&x, pb), connectionExists);             /* matrix 305 */
}

static void active_open_same_quad_duplicate_socket(void) {
    pbt x;
    TCPiopb *a, *b;
    StreamPtr s1, s2;
    pbt_init(&x, 1);
    s1 = pbt_create(&x, 4096, 0, 0);
    s2 = pbt_create(&x, 4096, 0, 0);
    a = pbt_open(&x, s1, 33333);
    T_CHECK(a != NULL);
    b = pbt_pb(TCPActiveOpen, s2);
    b->csParam.open.remoteHost = T_PEER;
    b->csParam.open.remotePort = PBT_PEER_PORT;
    b->csParam.open.localPort = 33333;
    T_EQ(pbt_call(&x, b), duplicateSocket);               /* matrix 306 */
}

static void active_open_closed_port_error_code(void) {
    pbt x;
    TCPiopb *pb;
    StreamPtr s;
    tcpv v;
    pbt_init(&x, 1);
    s = pbt_create(&x, 4096, pbf_asr(asr_nop), 0);
    fake_clear(&x.fl);
    pb = pbt_open(&x, s, 0);
    T_CHECK(pb && pbt_last_tcp(&x, &v));
    if (!pb) return;
    pbt_peer(&x, v.sport, 0, v.seq + 1, TF_RST | TF_ACK, 0, NULL, 0);
    T_EQ(pb->ioResult, openFailed);                       /* matrix 301 */
    T_EQ(pbf_count('A'), 0);                              /* matrix 301: no ASR event */
    T_EQ(pbt_simple(&x, TCPStatus, s), connectionDoesntExist);   /* matrix 301 st.res */
    pb = pbt_pb(TCPActiveOpen, s);                        /* remote port 0 */
    pb->csParam.open.remoteHost = T_PEER;
    T_EQ(pbt_call(&x, pb), openFailed);                   /* matrix 304b (MacTCP: after 3 ticks) */
    pb = pbt_pb(TCPActiveOpen, s);                        /* remote host 0 */
    pb->csParam.open.remotePort = 23;
    T_EQ(pbt_call(&x, pb), openFailed);                   /* matrix 304 (MacTCP: after the timeout) */
    T_EQ(pbt_simple(&x, TCPActiveOpen, 0), invalidStreamPtr);    /* matrix 304c */
}

static void active_open_ulp_timeout_open_failed(void) {
    pbt x;
    TCPiopb *pb;
    StreamPtr s;
    uint32_t t;
    pbt_init(&x, 1);
    s = pbt_create(&x, 4096, pbf_asr(asr_nop), 0);
    pb = pbt_pb(TCPActiveOpen, s);
    pb->csParam.open.remoteHost = T_PEER;
    pb->csParam.open.remotePort = PBT_PEER_PORT;
    pb->csParam.open.validityFlags = (int8_t)(timeoutValue | timeoutAction);
    pb->csParam.open.ulpTimeoutValue = 8;
    pb->csParam.open.ulpTimeoutAction = 1;
    T_EQ(pbt_call(&x, pb), inProgress);
    for (t = 0; t < 20000 && pb->ioResult == inProgress; t += 100) {
        pbf_now += 100;
        if (mt_pb_next_deadline_ms(x.d) == 0) mt_pb_tick(x.d);
    }
    T_EQ(pb->ioResult, openFailed);                       /* matrix 302: -23015 after the ULP timeout */
    T_CHECK(t >= 7900 && t <= 8100);
    T_EQ(pbf_count('A'), 0);                              /* matrix 302: no ASR */
}

static void send_push_leaves_before_return(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s;
    TCPiopb *pb;
    tcpv v;
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    T_CHECK(s != 0);
    pb = pbt_send(&x, s, "hello", 5, 1);
    T_EQ(pb->ioResult, inProgress);                       /* matrix 400: completes on the ACK */
    T_CHECK(pbt_last_tcp(&x, &v));                        /* the data left before the call returned */
    T_EQ(v.dlen, 5);
    T_MEMEQ(v.data, "hello", 5);
    T_EQ(v.seq, c.ms);
    T_CHECK(v.flags & TF_PSH);
    fake_clear(&x.fl);
    pb = pbt_send(&x, s, "world", 5, 0);                  /* push 0 behind unacked data: queued */
    T_EQ(pb->ioResult, inProgress);
    T_EQ(x.fl.n, 0);                                      /* (Nagle holds it until the ACK) */
    pbt_peer(&x, c.lport, c.ps, c.ms + 5, TF_ACK, 8192, NULL, 0);
    T_CHECK(pbt_last_tcp(&x, &v) && v.dlen == 5 && v.seq == c.ms + 5);
}

static void send_completes_after_ack(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s;
    TCPiopb *a, *b, *st;
    int n0;
    static uint8_t big[3000];
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    a = pbt_send(&x, s, "abcde", 5, 1);
    memset(big, 'B', sizeof big);
    b = pbt_send(&x, s, big, sizeof big, 1);              /* matrix 406: a second send is queued */
    T_EQ(a->ioResult, inProgress);
    T_EQ(b->ioResult, inProgress);
    pbf_now += 10;
    pbt_peer(&x, c.lport, c.ps, c.ms + 3, TF_ACK, 8192, NULL, 0);   /* partial ACK: nothing done */
    T_EQ(a->ioResult, inProgress);
    n0 = pbf_nlog;
    pbt_peer(&x, c.lport, c.ps, c.ms + 5, TF_ACK, 8192, NULL, 0);   /* all of A */
    T_EQ(a->ioResult, 0);                                 /* matrix 400: on the ACK, inside that input */
    T_EQ(pbf_find('C', pbf_a(a), n0), n0);
    T_EQ(b->ioResult, inProgress);
    st = pbt_pb(TCPStatus, s);
    T_EQ(pbt_call(&x, st), 0);
    T_EQ(st->csParam.status.amtUnackedData, 3000);
    pbt_peer(&x, c.lport, c.ps, c.ms + 5 + 3000, TF_ACK, 8192, NULL, 0);
    T_EQ(b->ioResult, 0);                                 /* matrix 406: in order, both 0 */
    T_CHECK(pbf_find('C', pbf_a(a), 0) < pbf_find('C', pbf_a(b), 0));
    T_EQ(pbt_call(&x, st), 0);
    T_EQ(st->csParam.status.amtUnackedData, 0);           /* matrix 400: 0 when the send completed */
}

static void rcv_pending_then_data_completes_same_input_call(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s;
    TCPiopb *r;
    int n0;
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    r = pbt_rcv(&x, s, 100, 0);
    T_EQ(r->ioResult, inProgress);
    n0 = pbf_nlog;
    pbt_peer(&x, c.lport, c.ps, c.ms, TF_ACK | TF_PSH, 8192, "12345", 5);
    T_EQ(r->ioResult, 0);
    T_EQ(r->csParam.receive.rcvBuffLen, 5);               /* matrix 500: as soon as any data */
    T_MEMEQ(pbf_p(r->csParam.receive.rcvBuff), "12345", 5);
    T_EQ(pbf_find('C', pbf_a(r), n0), n0);
    T_EQ(pbf_count('A'), 0);                              /* matrix 400/502: no DataArrival with a read pending */
    T_EQ(r->csParam.receive.markFlag, 0);                 /* matrix 500 */
}

static void data_arrival_asr_fires_inside_input(void) {
    static const char banner[] = "abcdefghijklmnopqrstuvwxyz";
    pbt x;
    pbt_conn c;
    StreamPtr s;
    TCPiopb *r;
    int n0;
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0xABCD, &c);
    n0 = pbf_nlog;
    pbt_peer(&x, c.lport, c.ps, c.ms, TF_ACK | TF_PSH, 8192, banner, 26);
    T_EQ(pbf_nlog, n0 + 1);
    T_EQ(pbf_log[n0].kind, 'A');
    T_EQ(pbf_log[n0].ev, TCPDataArrival);
    T_EQ(pbf_log[n0].reason, 26);                         /* matrix Events: terminReason = byte count (504) */
    T_EQ(pbf_log[n0].user, 0xABCD);                       /* matrix Events: TCPCreate's userDataPtr */
    T_EQ(pbf_log[n0].stream, s);
    r = pbt_rcv(&x, s, 10, 0);
    T_EQ(r->ioResult, 0);                                 /* matrix 500: 0 with 10 */
    T_EQ(r->csParam.receive.rcvBuffLen, 10);
    r = pbt_rcv(&x, s, 100, 0);
    T_EQ(r->ioResult, 0);                                 /* matrix 500: then 0 with 16 */
    T_EQ(r->csParam.receive.rcvBuffLen, 16);
    T_MEMEQ(pbf_p(r->csParam.receive.rcvBuff), banner + 10, 16);
    r = pbt_rcv(&x, s, 100, 2);                           /* nothing left, 2 s timeout */
    T_EQ(r->ioResult, inProgress);
    pbf_now += 1999;
    mt_pb_tick(x.d);
    T_EQ(r->ioResult, inProgress);
    pbf_now += 1;
    T_EQ(mt_pb_next_deadline_ms(x.d), 0);
    mt_pb_tick(x.d);
    T_EQ(r->ioResult, commandTimeout);                    /* matrix 500: -23016 */
}

static void nocopy_rcv_then_return_bfr(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s;
    TCPiopb *r, *st, *ret;
    rdsEntry *rds, e[3];
    TCPiopb *cr;
    uint8_t *rb;
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    pbt_peer(&x, c.lport, c.ps, c.ms, TF_ACK | TF_PSH, 8192, "abcdefghijklmnopqrstuvwxyz", 26);
    rds = (rdsEntry *)pbf_alloc(6 * sizeof(rdsEntry));
    r = pbt_pb(TCPNoCopyRcv, s);
    r->csParam.receive.rdsPtr = pbf_a(rds);
    r->csParam.receive.rdsLength = 6;
    T_EQ(pbt_call(&x, r), 0);                             /* matrix 504 */
    T_EQ(r->csParam.receive.rdsLength, 6);                /* matrix 504: stays the array size */
    memcpy(e, rds, sizeof e);
    T_EQ(e[0].length, 26);
    T_EQ(e[1].length, 0);                                 /* matrix 504: entry 1 terminates */
    /* the pointer is inside the stream's receive buffer (matrix 504) */
    cr = pbt_pb(TCPStatus, s);
    T_EQ(pbt_call(&x, cr), 0);
    T_EQ(cr->csParam.status.amtUnreadData, 0);            /* lent data counts as read */
    T_MEMEQ(pbf_p(e[0].ptr), "abcdefghijklmnopqrstuvwxyz", 26);
    {
        mt_tcp_info in;
        mt_tcp_get_info(x.d->tcp[0].t, &in);
        rb = (uint8_t *)pbf_p(x.d->tcp[0].rbuf);
        T_CHECK((uint8_t *)pbf_p(e[0].ptr) >= rb && (uint8_t *)pbf_p(e[0].ptr) + 26 <= rb + 8192);
        T_EQ(in.rcv_avail, 26);                           /* the core keeps the bytes until returned */
    }
    ret = pbt_pb(TCPRcvBfrReturn, s);
    ret->csParam.receive.rdsPtr = pbf_a(rds);
    T_EQ(pbt_call(&x, ret), 0);                           /* matrix 504: return 0 */
    {
        mt_tcp_info in;
        mt_tcp_get_info(x.d->tcp[0].t, &in);
        T_EQ(in.rcv_avail, 0);                            /* consumed now */
    }
    ret = pbt_pb(TCPRcvBfrReturn, s);
    ret->csParam.receive.rdsPtr = pbf_a(rds);
    T_EQ(pbt_call(&x, ret), invalidRDS);                  /* matrix 504: again -> -23014 */
    st = pbt_pb(TCPNoCopyRcv, s);                         /* pending no-copy read, data later */
    st->csParam.receive.rdsPtr = pbf_a(rds);
    st->csParam.receive.rdsLength = 6;
    T_EQ(pbt_call(&x, st), inProgress);
    pbt_peer(&x, c.lport, c.ps + 26, c.ms, TF_ACK | TF_PSH, 8192, "XY", 2);
    T_EQ(st->ioResult, 0);
    memcpy(e, rds, sizeof e);
    T_EQ(e[0].length, 2);
    T_MEMEQ(pbf_p(e[0].ptr), "XY", 2);
}

static void nocopy_bad_return_error(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s;
    TCPiopb *r;
    rdsEntry *rds, e[2];
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    pbt_peer(&x, c.lport, c.ps, c.ms, TF_ACK | TF_PSH, 8192, "abc", 3);
    r = pbt_pb(TCPNoCopyRcv, s);                          /* null RDS */
    r->csParam.receive.rdsLength = 6;
    T_EQ(pbt_call(&x, r), invalidBufPtr);                 /* matrix 992 */
    rds = (rdsEntry *)pbf_alloc(2 * sizeof(rdsEntry));
    e[0].length = 3;                                      /* made up: points at our own memory */
    e[0].ptr = pbf_a(rds);
    e[1].length = 0;
    e[1].ptr = 0;
    memcpy(rds, e, sizeof e);
    r = pbt_pb(TCPRcvBfrReturn, s);
    r->csParam.receive.rdsPtr = pbf_a(rds);
    T_EQ(pbt_call(&x, r), invalidRDS);                    /* matrix 504: a made-up RDS -> -23014 */
    r = pbt_rcv(&x, s, 10, 0);                            /* the data is still there */
    T_EQ(r->ioResult, 0);
    T_EQ(r->csParam.receive.rcvBuffLen, 3);
    T_EQ(pbt_simple(&x, TCPRcvBfrReturn, 0), invalidStreamPtr);  /* matrix 120.36 */
}

static void rcv_and_send_argument_errors(void) {
    pbt x;
    StreamPtr s;
    TCPiopb *r;
    pbt_init(&x, 1);
    s = pbt_create(&x, 4096, 0, 0);
    r = pbt_pb(TCPRcv, s);                                /* null buffer, not connected */
    r->csParam.receive.rcvBuffLen = 10;
    T_EQ(pbt_call(&x, r), invalidBufPtr);                 /* matrix 991: checked before the state */
    r = pbt_rcv(&x, s, 0, 0);
    T_EQ(r->ioResult, invalidLength);                     /* matrix 501 */
    r = pbt_rcv(&x, s, 10, 0);
    T_EQ(r->ioResult, connectionDoesntExist);             /* matrix 503 */
    r = pbt_send(&x, s, "x", 1, 1);
    T_EQ(r->ioResult, connectionDoesntExist);             /* matrix 405 */
    T_EQ(pbt_simple(&x, TCPClose, s), connectionDoesntExist);   /* matrix 602, twice */
    T_EQ(pbt_simple(&x, TCPClose, s), connectionDoesntExist);
    T_EQ(pbt_simple(&x, TCPAbort, s), connectionDoesntExist);   /* matrix 602 */
    T_EQ(pbt_simple(&x, TCPAbort, 0), invalidStreamPtr);        /* matrix 604 */
    T_EQ(pbt_simple(&x, TCPExtendedStat, 0), invalidStreamPtr); /* matrix sweep note */
}

/* More TCPSend PBs than the core queues (2): the glue holds the rest and merges them into one
 * core request when a slot frees, yet completes each PB when its own last byte is ACKed. */
static void send_merge_completes_each_pb(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s;
    TCPiopb *pb[6];
    static uint8_t d[6][100];
    uint32_t off[7];
    tcpv v;
    int i, k;
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    off[0] = c.ms;
    for (i = 0; i < 6; i++) {
        uint16_t len = (uint16_t)(i < 2 ? 100 : 50);
        memset(d[i], 'a' + i, sizeof d[i]);
        pb[i] = pbt_send(&x, s, d[i], len, 1);
        off[i + 1] = off[i] + len;
    }
    T_EQ(x.d->tcp[0].req_n, 2);                           /* two core requests, four PBs held */
    T_EQ(x.d->tcp[0].sq_n, 6);
    fake_clear(&x.fl);
    pbt_peer(&x, c.lport, c.ps, off[1], TF_ACK, 8192, NULL, 0);
    T_EQ(pb[0]->ioResult, 0);
    T_EQ(pb[1]->ioResult, inProgress);
    T_CHECK(pbt_last_tcp(&x, &v));                        /* PBs 3..6 left as one 200-byte segment */
    T_EQ(v.seq, off[2]);
    T_EQ(v.dlen, 200);
    for (k = 0; k < 200 && k < v.dlen; k++) T_EQ(v.data[k], 'c' + k / 50);
    pbt_peer(&x, c.lport, c.ps, off[2], TF_ACK, 8192, NULL, 0);
    T_EQ(pb[1]->ioResult, 0);
    T_EQ(pb[2]->ioResult, inProgress);
    pbt_peer(&x, c.lport, c.ps, off[4] - 1, TF_ACK, 8192, NULL, 0);   /* inside the merged request */
    T_EQ(pb[2]->ioResult, 0);                             /* its own last byte is ACKed */
    T_EQ(pb[3]->ioResult, inProgress);
    pbt_peer(&x, c.lport, c.ps, off[4], TF_ACK, 8192, NULL, 0);
    T_EQ(pb[3]->ioResult, 0);
    T_EQ(pb[4]->ioResult, inProgress);
    pbt_peer(&x, c.lport, c.ps, off[6], TF_ACK, 8192, NULL, 0);
    T_EQ(pb[4]->ioResult, 0);
    T_EQ(pb[5]->ioResult, 0);
    for (i = 0; i + 1 < 6; i++)
        T_CHECK(pbf_find('C', pbf_a(pb[i]), 0) < pbf_find('C', pbf_a(pb[i + 1]), 0));
    T_EQ(x.d->tcp[0].req_n, 0);
    T_EQ(x.d->tcp[0].sq_n, 0);
}

/* A WDS with more entries than one core request takes (MT_SEGS_MAX) goes out in order. */
static void send_wds_more_than_8_entries(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s;
    TCPiopb *pb;
    wdsEntry *w, e;
    uint8_t *b, got[120];
    int i, n = 0, f;
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    w = (wdsEntry *)pbf_alloc(13 * sizeof(wdsEntry));
    b = (uint8_t *)pbf_alloc(240);
    for (i = 0; i < 12; i++) {
        memset(b + 20 * i + (i & 1), 'A' + i, 10);
        e.length = 10;
        e.ptr = pbf_a(b + 20 * i + (i & 1));
        memcpy(w + i, &e, sizeof e);
    }
    pb = pbt_pb(TCPSend, s);
    pb->csParam.send.wdsPtr = pbf_a(w);
    pb->csParam.send.pushFlag = 1;
    T_EQ(pbt_call(&x, pb), inProgress);
    for (f = 0; f < x.fl.n; f++) {
        tcpv v;
        if (view_tcp(x.fl.f[f], x.fl.len[f], &v) && v.dlen && n + v.dlen <= 120) {
            memcpy(got + n, v.data, v.dlen);
            n += v.dlen;
        }
    }
    T_EQ(n, 120);
    for (i = 0; i < 120 && i < n; i++) T_EQ(got[i], 'A' + i / 10);
    pbt_peer(&x, c.lport, c.ps, c.ms + 80, TF_ACK, 8192, NULL, 0);
    T_EQ(pb->ioResult, inProgress);
    pbt_peer(&x, c.lport, c.ps, c.ms + 120, TF_ACK, 8192, NULL, 0);
    T_EQ(pb->ioResult, 0);
}

/* An ICMP error about a connection reaches the ASR as TCPICMPReceived with an ICMPReport that
 * is valid during the call (the payload is OPEN in the matrix: never produced in Snow). */
static ICMPReport g_rep;
static int g_icmp_asr;
static void asr_icmp(StreamPtr s, uint16_t ev, MacPtr32 user, uint16_t reason, MacPtr32 icmp) {
    (void)s; (void)user; (void)reason;
    if (ev != TCPICMPReceived) return;
    g_icmp_asr++;
    if (pbf_p(icmp)) memcpy(&g_rep, pbf_p(icmp), sizeof g_rep);
}

static void icmp_report_asr(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s;
    uint8_t m[36], f[120];
    uint32_t sum = 0;
    uint16_t n, i;
    pbt_init(&x, 1);
    g_icmp_asr = 0;
    s = pbt_est(&x, pbf_asr(asr_icmp), 0, &c);
    memset(m, 0, sizeof m);
    m[0] = 3;                                             /* destination unreachable, port */
    m[1] = 3;
    m[8] = 0x45;                                          /* quoted header: our datagram to the peer */
    m[8 + 9] = 6;
    mt_wr32(m + 8 + 12, T_ME);
    mt_wr32(m + 8 + 16, T_PEER);
    mt_wr16(m + 28, c.lport);
    mt_wr16(m + 30, PBT_PEER_PORT);
    for (i = 0; i < sizeof m; i += 2) sum += (uint32_t)(m[i] << 8 | m[i + 1]);
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    mt_wr16(m + 2, (uint16_t)~sum);
    n = mk_ip(f, T_PEER, T_ME, 1, 77, 0, m, sizeof m);
    mt_pb_input(x.d, f, n);
    T_EQ(g_icmp_asr, 1);
    T_EQ(g_rep.streamPtr, s);
    T_EQ(g_rep.reportType, portUnreach);
    T_EQ(g_rep.localPort, c.lport);
    T_EQ(g_rep.remoteHost, T_PEER);
    T_EQ(g_rep.remotePort, PBT_PEER_PORT);
    T_EQ(pbt_simple(&x, TCPStatus, s), 0);                /* reported, not aborted */
}

T_MAIN(create_min_buffer_rejected, create_release_roundtrip, active_open_completes_on_established,
       active_open_same_quad_duplicate_socket, active_open_closed_port_error_code,
       active_open_ulp_timeout_open_failed, send_push_leaves_before_return,
       send_completes_after_ack, rcv_pending_then_data_completes_same_input_call,
       data_arrival_asr_fires_inside_input, nocopy_rcv_then_return_bfr, nocopy_bad_return_error,
       rcv_and_send_argument_errors, send_merge_completes_each_pb, send_wds_more_than_8_entries,
       icmp_report_asr)
