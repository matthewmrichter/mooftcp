/* PB dispatcher, TCP calls (part 2): close/abort/release ordering, re-entrancy, queued reads,
 * status, passive open, unconfigured driver, WDS edge cases, code tables, csCode sweep.
 * Matrix case ids: docs/dev/MACTCP-REFERENCE-MATRIX.md. */
#include "t.h"
#include "pbfake.h"

static void asr_nop(StreamPtr s, uint16_t ev, MacPtr32 user, uint16_t reason, MacPtr32 icmp) {
    (void)s; (void)ev; (void)user; (void)reason; (void)icmp;
}

static void close_with_pending_rcv_completes_rcv(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s;
    TCPiopb *r, *cl, *st;
    tcpv v;
    int i, a;
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    r = pbt_rcv(&x, s, 100, 0);
    cl = pbt_pb(TCPClose, s);
    T_EQ(pbt_call(&x, cl), inProgress);
    T_CHECK(pbt_last_tcp(&x, &v) && (v.flags & TF_FIN));
    pbt_peer(&x, c.lport, c.ps, c.ms + 1, TF_ACK, 8192, NULL, 0);   /* our FIN acked: FIN_WAIT_2 */
    T_EQ(cl->ioResult, 0);                                /* matrix 600: TCPClose completes 0 */
    T_EQ(r->ioResult, inProgress);                        /* matrix 600: the read stays pending */
    st = pbt_pb(TCPStatus, s);
    T_EQ(pbt_call(&x, st), 0);
    T_EQ(st->csParam.status.connectionState, ConnectionStateFINWait2);   /* matrix 600: state 12 */
    T_EQ(st->csParam.status.amtUnackedData, 0);           /* matrix 600: st.unack 0 (FIN acked) */
    T_EQ(pbt_send(&x, s, "z", 1, 1)->ioResult, connectionClosing);      /* matrix 408 */
    T_EQ(pbt_simple(&x, TCPClose, s), connectionClosing);
    T_EQ(pbt_simple(&x, TCPRelease, s), 0);               /* matrix 600/605: then -23012 */
    T_EQ(r->ioResult, connectionTerminated);
    i = pbf_find('C', pbf_a(r), 0);
    a = -1;
    for (a = i; a < pbf_nlog && pbf_log[a].kind != 'A'; a++) {}
    T_CHECK(i >= 0 && a < pbf_nlog);
    T_EQ(pbf_log[a].ev, TCPTerminate);                    /* matrix 600: A:e3:t6 after the read */
    T_EQ(pbf_log[a].reason, TCPULPAbort);
}

static void peer_close_closing_after_data_read(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s;
    TCPiopb *r, *st;
    int i;
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    i = pbf_nlog;
    pbt_peer(&x, c.lport, c.ps, c.ms, TF_ACK | TF_PSH | TF_FIN, 8192, "seventeen bytes!!", 17);
    T_EQ(pbf_nlog, i + 1);                                /* matrix 601: data arrival only */
    T_EQ(pbf_log[i].ev, TCPDataArrival);
    T_EQ(pbf_log[i].reason, 17);
    st = pbt_pb(TCPStatus, s);
    T_EQ(pbt_call(&x, st), 0);
    T_EQ(st->csParam.status.amtUnreadData, 17);           /* matrix 601: 17 unread */
    T_EQ(st->csParam.status.connectionState, ConnectionStateEstablished);   /* matrix 601: 8 until read */
    r = pbt_rcv(&x, s, 100, 0);                           /* drains: completes 0, then TCPClosing */
    T_EQ(r->ioResult, 0);                                 /* matrix 601: rcv1 0, n1 17 */
    T_EQ(r->csParam.receive.rcvBuffLen, 17);
    i = pbf_find('C', pbf_a(r), 0);
    T_CHECK(i >= 0 && i + 1 < pbf_nlog);
    if (i >= 0 && i + 1 < pbf_nlog) {
        T_EQ(pbf_log[i + 1].kind, 'A');                   /* matrix 601: C:k1:r0 then A:e1 */
        T_EQ(pbf_log[i + 1].ev, TCPClosing);
    }
    T_EQ(pbt_rcv(&x, s, 100, 0)->ioResult, connectionClosing);   /* matrix 601: rcv2 -23005 */
    T_EQ(pbt_call(&x, st), 0);
    T_EQ(st->csParam.status.connectionState, ConnectionStateCloseWait);   /* matrix 601: 14 */
    T_EQ(pbt_simple(&x, TCPClose, s), inProgress);        /* LAST_ACK */
    pbt_peer(&x, c.lport, c.ps + 18, c.ms + 1, TF_ACK, 8192, NULL, 0);
    T_EQ(pbf_log[pbf_nlog - 1].kind, 'A');                /* matrix 601/606: close completes, then */
    T_EQ(pbf_log[pbf_nlog - 1].ev, TCPTerminate);         /* TCPTerminate reason 7 */
    T_EQ(pbf_log[pbf_nlog - 1].reason, TCPULPClose);
    T_EQ(pbf_log[pbf_nlog - 2].kind, 'C');
    T_EQ(pbf_log[pbf_nlog - 2].res, 0);
    T_EQ(pbt_simple(&x, TCPStatus, s), connectionDoesntExist);   /* matrix 601: afterClose -23008 */
}

static void abort_connected_pending_rcv_order(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s;
    TCPiopb *r, *ab;
    int n0;
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    r = pbt_rcv(&x, s, 100, 0);
    n0 = pbf_nlog;
    ab = pbt_pb(TCPAbort, s);
    T_EQ(pbt_call(&x, ab), 0);                            /* matrix 603 */
    T_EQ(pbf_nlog, n0 + 3);                               /* matrix 603: C(rcv -23012) A(e3 t6) C(abort 0) */
    T_EQ(pbf_log[n0].kind, 'C');
    T_EQ(pbf_log[n0].pb, pbf_a(r));
    T_EQ(pbf_log[n0].res, connectionTerminated);
    T_EQ(pbf_log[n0 + 1].kind, 'A');
    T_EQ(pbf_log[n0 + 1].ev, TCPTerminate);
    T_EQ(pbf_log[n0 + 1].reason, TCPULPAbort);
    T_EQ(pbf_log[n0 + 2].pb, pbf_a(ab));
    T_EQ(pbt_simple(&x, TCPStatus, s), connectionDoesntExist);   /* matrix 603 */
}

static void release_with_everything_pending_completes_all_pbs(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s;
    TCPiopb *snd[5], *rcv[2], *rel;
    static uint8_t data[2000];
    int i, n0, asr_at = -1, rel_at = -1;
    mt_tcp_info in;
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    for (i = 0; i < 5; i++) snd[i] = pbt_send(&x, s, data, (uint16_t)(400 * (i + 1)), i & 1);
    for (i = 0; i < 2; i++) rcv[i] = pbt_rcv(&x, s, 50, 0);
    for (i = 0; i < 5; i++) T_EQ(snd[i]->ioResult, inProgress);
    T_EQ(rcv[1]->ioResult, inProgress);
    n0 = pbf_nlog;
    rel = pbt_pb(TCPRelease, s);
    T_EQ(pbt_call(&x, rel), 0);                           /* matrix 605 */
    for (i = 0; i < 5; i++) T_EQ(snd[i]->ioResult, connectionTerminated);
    for (i = 0; i < 2; i++) T_EQ(rcv[i]->ioResult, connectionTerminated);   /* matrix 605 */
    for (i = n0; i < pbf_nlog; i++) {
        if (pbf_log[i].kind == 'A') asr_at = i;
        if (pbf_log[i].kind == 'C' && pbf_log[i].pb == pbf_a(rel)) rel_at = i;
    }
    T_EQ(pbf_nlog - n0, 7 + 1 + 1);                       /* 7 PBs once each, one ASR, the release */
    T_EQ(asr_at, pbf_nlog - 2);                           /* matrix Events: PBs before TCPTerminate */
    T_EQ(rel_at, pbf_nlog - 1);
    T_EQ(pbf_log[asr_at].reason, TCPULPAbort);            /* matrix 605: reason 6 */
    for (i = n0; i + 1 < asr_at; i++)                     /* sends complete in submission order */
        if (pbf_log[i].cs == TCPSend && pbf_log[i + 1].cs == TCPSend)
            T_CHECK(pbf_log[i].pb < pbf_log[i + 1].pb);
    T_EQ(pbt_simple(&x, TCPStatus, s), invalidStreamPtr);   /* matrix 605: statusAfter -23010 */
    T_EQ(x.d->tcp[0].used, 0);                            /* the core TCB is free again */
    T_EQ(mt_tcp_count_open(x.d->s), 0);
    n0 = pbf_nlog;
    pbt_peer(&x, c.lport, c.ps, c.ms + 400, TF_ACK, 8192, NULL, 0);   /* late ACK: nothing fires */
    T_EQ(pbf_nlog, n0);
    T_CHECK(pbt_create(&x, 4096, 0, 0) != 0);
    mt_tcp_get_info(x.d->tcp[1].t ? x.d->tcp[1].t : x.d->tcp[0].t, &in);
    T_EQ(in.state, MT_TCP_CLOSED);
    T_EQ(pbf_bad, 0);
}

/* ---- re-entrancy: the app calls the driver from its ASR and completion routines ---- */
static pbt *g_x;
static StreamPtr g_s;
static TCPiopb *g_rd, *g_sd;
static int g_asr_calls, g_release_in_asr;
static int16_t g_rel_res;

static void asr_reenter(StreamPtr s, uint16_t ev, MacPtr32 user, uint16_t reason, MacPtr32 icmp) {
    (void)user; (void)reason; (void)icmp;
    g_asr_calls++;
    if (ev != TCPDataArrival || s != g_s) return;
    g_rd = pbt_rcv(g_x, s, 100, 0);                       /* read inside the ASR */
    g_sd = pbt_send(g_x, s, "echo", 4, 1);                /* reply inside the ASR */
    if (g_release_in_asr) g_rel_res = pbt_simple(g_x, TCPRelease, s);
}

static void call_from_inside_asr_ok(void) {
    pbt x;
    pbt_conn c;
    tcpv v;
    g_x = &x;
    g_asr_calls = 0;
    g_release_in_asr = 0;
    pbt_init(&x, 1);
    g_s = pbt_est(&x, pbf_asr(asr_reenter), 0, &c);
    pbt_peer(&x, c.lport, c.ps, c.ms, TF_ACK | TF_PSH, 8192, "ping", 4);
    T_EQ(g_asr_calls, 1);
    T_EQ(g_rd->ioResult, 0);                              /* the read got the data at once */
    T_EQ(g_rd->csParam.receive.rcvBuffLen, 4);
    T_EQ(g_sd->ioResult, inProgress);
    T_CHECK(pbt_last_tcp(&x, &v) && v.dlen == 4);         /* the reply carries the ACK */
    T_EQ(v.ack, c.ps + 4);
    T_EQ(pbf_irq, 0);
    /* release from inside the ASR: deferred core release, PBs completed, stream gone */
    g_release_in_asr = 1;
    pbt_peer(&x, c.lport, c.ps + 4, c.ms + 4, TF_ACK | TF_PSH, 8192, "again", 5);
    T_EQ(g_rel_res, 0);
    T_EQ(g_sd->ioResult, connectionTerminated);
    T_EQ(pbt_simple(&x, TCPStatus, g_s), invalidStreamPtr);
    T_EQ(x.d->rel_mask, 0);                               /* the deferred release ran */
    T_EQ(x.d->tcp[0].used, 0);
    T_EQ(mt_tcp_count_open(x.d->s), 0);
    T_EQ(pbf_bad, 0);
    T_EQ(pbf_irq, 0);
    T_EQ(x.d->depth, 0);
}

static void second_rcv_queued_per_matrix(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s;
    TCPiopb *r1, *r2;
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    r1 = pbt_rcv(&x, s, 100, 0);
    r2 = pbt_rcv(&x, s, 100, 0);
    T_EQ(r1->ioResult, inProgress);                       /* matrix 502: both accepted, pending */
    T_EQ(r2->ioResult, inProgress);
    pbt_peer(&x, c.lport, c.ps, c.ms, TF_ACK | TF_PSH, 8192, "ab", 2);
    T_EQ(r1->ioResult, 0);                                /* matrix 502: the first gets 2 bytes */
    T_EQ(r1->csParam.receive.rcvBuffLen, 2);
    T_EQ(r2->ioResult, inProgress);                       /* matrix 502: the second stays pending */
    pbt_peer(&x, c.lport, c.ps + 2, c.ms, TF_ACK | TF_PSH, 8192, "cd", 2);
    T_EQ(r2->ioResult, 0);
    T_EQ(r2->csParam.receive.rcvBuffLen, 2);
    T_MEMEQ(pbf_p(r2->csParam.receive.rcvBuff), "cd", 2);
    T_EQ(pbf_count('A'), 0);                              /* no DataArrival while reads are pending */
}

static void status_fields(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s;
    TCPiopb *st;
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0x55, &c);
    pbt_send(&x, s, "0123456789", 10, 1);
    pbt_peer(&x, c.lport, c.ps, c.ms, TF_ACK | TF_PSH, 6000, "abc", 3);
    st = pbt_pb(TCPStatus, s);
    st->csParam.status.userDataPtr = 0x77;
    T_EQ(pbt_call(&x, st), 0);
    T_EQ(st->csParam.status.remoteHost, T_PEER);
    T_EQ(st->csParam.status.remotePort, PBT_PEER_PORT);
    T_EQ(st->csParam.status.localHost, T_ME);
    T_EQ(st->csParam.status.localPort, c.lport);
    T_EQ(st->csParam.status.connectionState, ConnectionStateEstablished);
    T_EQ(st->csParam.status.sendWindow, 6000);
    T_EQ(st->csParam.status.rcvWindow, 8192 - 3);         /* the whole buffer is the window */
    T_EQ(st->csParam.status.amtUnackedData, 10);
    T_EQ(st->csParam.status.amtUnreadData, 3);
    T_EQ(st->csParam.status.sendUnacked, c.ms);
    T_EQ(st->csParam.status.sendNext, c.ms + 10);
    T_EQ(st->csParam.status.rcvNext, c.ps + 3);
    T_EQ(st->csParam.status.sendMaxSegSize, 1460);
    T_CHECK(st->csParam.status.connStatPtr != 0);
    T_EQ(st->csParam.status.userDataPtr, 0x77);           /* the app's own field is left alone */
    T_EQ(st->csParam.status.ulpTimeoutValue, (int8_t)120);
    T_EQ(st->csParam.status.ulpTimeoutAction, 1);
}

static void passive_open_accepts(void) {
    pbt x;
    StreamPtr s;
    TCPiopb *po, *st;
    tcpv v;
    pbt_init(&x, 1);
    s = pbt_create(&x, 4096, pbf_asr(asr_nop), 0);
    po = pbt_pb(TCPPassiveOpen, s);
    po->csParam.open.localPort = 7000;
    T_EQ(pbt_call(&x, po), inProgress);
    st = pbt_pb(TCPStatus, s);
    T_EQ(pbt_call(&x, st), 0);
    T_EQ(st->csParam.status.connectionState, ConnectionStateListening);
    fake_clear(&x.fl);
    pbt_peer(&x, 7000, 9000, 0, TF_SYN, 8192, NULL, 0);
    T_CHECK(pbt_last_tcp(&x, &v) && v.flags == (TF_SYN | TF_ACK));
    T_EQ(po->ioResult, inProgress);
    pbt_peer(&x, 7000, 9001, v.seq + 1, TF_ACK, 8192, NULL, 0);
    T_EQ(po->ioResult, 0);
    T_EQ(po->csParam.open.remoteHost, T_PEER);
    T_EQ(po->csParam.open.remotePort, PBT_PEER_PORT);
    T_EQ(po->csParam.open.localHost, T_ME);
    T_EQ(po->csParam.open.localPort, 7000);
    T_EQ(pbt_call(&x, st), 0);
    T_EQ(st->csParam.status.connectionState, ConnectionStateEstablished);
}

static void passive_open_port0_per_matrix(void) {
    pbt x;
    StreamPtr s, s2;
    TCPiopb *po, *st, *po2;
    uint16_t lp;
    int n0;
    pbt_init(&x, 1);
    s = pbt_create(&x, 4096, pbf_asr(asr_nop), 0);
    po = pbt_pb(TCPPassiveOpen, s);
    po->csParam.open.validityFlags = (int8_t)timeoutValue;
    po->csParam.open.commandTimeoutValue = 4;
    T_EQ(pbt_call(&x, po), inProgress);
    st = pbt_pb(TCPStatus, s);
    T_EQ(pbt_call(&x, st), 0);
    T_EQ(st->csParam.status.connectionState, ConnectionStateListening);   /* matrix 700: state 2 */
    lp = st->csParam.status.localPort;
    T_CHECK(lp >= 1024 && lp <= 2047);                    /* matrix 700: ephemeral 1024-2047 */
    T_EQ(st->csParam.status.remoteHost, 0);               /* matrix 700: remote 0.0.0.0:0 */
    s2 = pbt_create(&x, 4096, pbf_asr(asr_nop), 0);      /* matrix 701: a second listen on that port */
    po2 = pbt_pb(TCPPassiveOpen, s2);
    po2->csParam.open.localPort = lp;
    T_EQ(pbt_call(&x, po2), inProgress);                  /* is accepted */
    pbf_now += 3990;
    mt_pb_tick(x.d);
    T_EQ(po->ioResult, inProgress);
    pbf_now += 10;
    n0 = pbf_nlog;
    T_EQ(mt_pb_next_deadline_ms(x.d), 0);
    mt_pb_tick(x.d);
    T_EQ(po->ioResult, commandTimeout);                   /* matrix 700: -23016 after ~4 s */
    T_EQ(po->csParam.open.localPort, lp);                 /* matrix 700: written back */
    T_EQ(po->csParam.open.localHost, T_ME);
    T_EQ(po->csParam.open.remoteHost, 0);
    T_EQ(pbf_log[n0].kind, 'C');
    T_EQ(pbf_log[n0 + 1].kind, 'A');                      /* matrix 701: TCPTerminate reason 6 */
    T_EQ(pbf_log[n0 + 1].ev, TCPTerminate);
    T_EQ(pbf_log[n0 + 1].reason, TCPULPAbort);
    T_EQ(pbt_simple(&x, TCPStatus, s), connectionDoesntExist);   /* matrix 700: after.res -23008 */
    T_EQ(po2->ioResult, inProgress);
    n0 = pbf_nlog;
    T_EQ(pbt_simple(&x, TCPAbort, s2), 0);                /* matrix 702: abort a pending passive open */
    T_EQ(po2->ioResult, connectionTerminated);
    T_EQ(pbf_log[n0].kind, 'C');
    T_EQ(pbf_log[n0 + 1].kind, 'A');
    T_EQ(pbf_log[n0 + 1].reason, TCPULPAbort);            /* matrix 702: reason 6 after the PB */
    T_EQ(pbt_simple(&x, TCPStatus, s2), connectionDoesntExist);  /* matrix 702: after.res */
}

static void unconfigured_returns_matrix_error(void) {
    pbt x;
    TCPiopb *pb;
    int16_t cs;
    uint8_t *buf;
    mt_ifconfig c;
    pbt_init(&x, 0);
    buf = (uint8_t *)pbf_alloc(4096);
    for (cs = 0; cs <= 60; cs++) {
        pb = pbt_pb(cs, 0);
        pb->csParam.create.rcvBuff = pbf_a(buf);
        pb->csParam.create.rcvBuffLen = 4096;
        T_EQ(mt_pb_control(x.d, (MtAnyPb *)pb), MT_PB_NOTOPEN);   /* matrix U01-U12, 120.*: -28 */
        T_EQ(pb->ioResult, MT_PB_NOTOPEN);
    }
    T_EQ(pbf_nlog, 0);                                    /* matrix: no completion routine runs */
    mt_get_config(x.d->s, &c);                            /* configure: the calls work */
    c.addr = T_ME;
    mt_pb_set_config(x.d, &c);
    pb = pbt_pb(TCPCreate, 0);
    pb->csParam.create.rcvBuff = pbf_a(buf);
    pb->csParam.create.rcvBuffLen = 4096;
    T_EQ(mt_pb_control(x.d, (MtAnyPb *)pb), 0);
    c.addr = 0;                                           /* and unconfigured again */
    mt_pb_set_config(x.d, &c);
    T_EQ(pbt_simple(&x, TCPStatus, pb->tcpStream), MT_PB_NOTOPEN);
}

static void zero_len_and_odd_wds_entries(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s;
    TCPiopb *pb;
    wdsEntry *w, e[5];
    uint8_t *b;
    tcpv v;
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    w = (wdsEntry *)pbf_alloc(5 * sizeof(wdsEntry));
    b = (uint8_t *)pbf_alloc(64);
    memcpy(b, "?abcdefghi?XYZ", 14);
    pb = pbt_pb(TCPSend, s);                              /* terminator only */
    pb->csParam.send.wdsPtr = pbf_a(w);
    T_EQ(pbt_call(&x, pb), invalidLength);                /* matrix 402b */
    e[0].length = 0; e[0].ptr = pbf_a(b);                 /* a zero-length entry ends the WDS */
    e[1].length = 3; e[1].ptr = pbf_a(b + 1);
    e[2].length = 0; e[2].ptr = 0;
    memcpy(w, e, 3 * sizeof(wdsEntry));
    pb = pbt_pb(TCPSend, s);
    pb->csParam.send.wdsPtr = pbf_a(w);
    T_EQ(pbt_call(&x, pb), invalidLength);                /* matrix 402 */
    pb = pbt_pb(TCPSend, s);                              /* null WDS */
    T_EQ(pbt_call(&x, pb), invalidWDS);                   /* matrix 990 */
    e[0].length = 2; e[0].ptr = pbf_a(b + 1);             /* 2 + 3 + 4 bytes at odd and even addresses */
    e[1].length = 3; e[1].ptr = pbf_a(b + 3);
    e[2].length = 4; e[2].ptr = pbf_a(b + 6);
    e[3].length = 0; e[3].ptr = 0;
    e[4].length = 3; e[4].ptr = pbf_a(b + 11);            /* after the terminator: not sent */
    memcpy(w, e, sizeof e);
    pb = pbt_pb(TCPSend, s);
    pb->csParam.send.wdsPtr = pbf_a(w);
    pb->csParam.send.pushFlag = 1;
    pb->csParam.send.sendLength = 3;                      /* ignored (matrix 404b) */
    fake_clear(&x.fl);
    T_EQ(pbt_call(&x, pb), inProgress);                   /* matrix 403/404: 0, bytes in order */
    T_CHECK(pbt_last_tcp(&x, &v));
    T_EQ(v.dlen, 9);
    T_MEMEQ(v.data, "abcdefghi", 9);
    pbt_peer(&x, c.lport, c.ps, c.ms + 9, TF_ACK, 8192, NULL, 0);
    T_EQ(pb->ioResult, 0);
}

static void code_tables_one_row_each(void) {
    /* terminate reasons (Events table, ref values) */
    T_EQ(mt_pb_term_reason(MT_TERM_REMOTE_ABORT), TCPRemoteAbort);
    T_EQ(mt_pb_term_reason(MT_TERM_NETWORK_FAILURE), TCPNetworkFailure);
    T_EQ(mt_pb_term_reason(MT_TERM_SEC_PREC_MISMATCH), TCPSecPrecMismatch);
    T_EQ(mt_pb_term_reason(MT_TERM_ULP_TIMEOUT), TCPULPTimeoutTerminate);
    T_EQ(mt_pb_term_reason(MT_TERM_ULP_ABORT), TCPULPAbort);         /* matrix 603/605 */
    T_EQ(mt_pb_term_reason(MT_TERM_ULP_CLOSE), TCPULPClose);         /* matrix 601 */
    T_EQ(mt_pb_term_reason(MT_TERM_SERVICE_ERROR), TCPServiceError);
    T_EQ(mt_pb_term_reason(MT_TERM_OPEN_FAILED), 0);                 /* matrix 301: no ASR */
    /* events */
    T_EQ(mt_pb_event_code(MT_TCP_EV_ESTABLISHED), 0);
    T_EQ(mt_pb_event_code(MT_TCP_EV_DATA), TCPDataArrival);
    T_EQ(mt_pb_event_code(MT_TCP_EV_URGENT), TCPUrgent);
    T_EQ(mt_pb_event_code(MT_TCP_EV_CLOSING), TCPClosing);
    T_EQ(mt_pb_event_code(MT_TCP_EV_ULP_TIMEOUT), TCPULPTimeout);
    T_EQ(mt_pb_event_code(MT_TCP_EV_TERMINATE), TCPTerminate);
    T_EQ(mt_pb_event_code(MT_TCP_EV_ICMP), TCPICMPReceived);
    /* ICMP report types */
    T_EQ(mt_pb_icmp_type(0x0300), netUnreach);
    T_EQ(mt_pb_icmp_type(0x0301), hostUnreach);
    T_EQ(mt_pb_icmp_type(0x0302), protocolUnreach);
    T_EQ(mt_pb_icmp_type(0x0303), portUnreach);
    T_EQ(mt_pb_icmp_type(0x0304), fragReqd);
    T_EQ(mt_pb_icmp_type(0x0305), sourceRouteFailed);
    T_EQ(mt_pb_icmp_type(0x030A), hostUnreach);
    T_EQ(mt_pb_icmp_type(0x0B00), timeExceeded);
    T_EQ(mt_pb_icmp_type(0x0C00), parmProblem);
}

static void remote_reset_and_ulp_timeout_reasons(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s;
    TCPiopb *r, *pb;
    uint32_t t;
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    r = pbt_rcv(&x, s, 10, 0);
    pbt_peer(&x, c.lport, c.ps, 0, TF_RST, 0, NULL, 0);
    T_EQ(r->ioResult, connectionTerminated);
    T_EQ(pbf_log[pbf_nlog - 1].ev, TCPTerminate);
    T_EQ(pbf_log[pbf_nlog - 1].reason, TCPRemoteAbort);   /* matrix OPEN (Snow hides RSTs): ref value */
    T_EQ(pbt_simple(&x, TCPRelease, s), 0);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);             /* ULP timeout: ASR 2, then terminate 5 */
    pb = pbt_send(&x, s, "lost", 4, 1);
    for (t = 0; t < 200000 && pb->ioResult == inProgress; t += 500) {
        pbf_now += 500;
        if (mt_pb_next_deadline_ms(x.d) == 0) mt_pb_tick(x.d);
    }
    T_EQ(pb->ioResult, connectionTerminated);
    T_EQ(pbf_log[pbf_nlog - 1].reason, TCPULPTimeoutTerminate);
    T_EQ(pbf_log[pbf_nlog - 2].kind, 'C');
    T_EQ(pbf_log[pbf_nlog - 3].ev, TCPULPTimeout);
}

static void cscode_sweep_bad_stream(void) {
    pbt x;
    int16_t cs;
    StreamPtr dead;
    int pass;
    pbt_init(&x, 1);
    dead = pbt_create(&x, 4096, 0, 0);
    T_EQ(pbt_simple(&x, TCPRelease, dead), 0);
    for (pass = 0; pass < 2; pass++) {                    /* matrix 120 (stream 0) and 121 (released) */
        StreamPtr sp = pass ? dead : 0;
        for (cs = 0; cs <= 60; cs++) {
            int16_t r;
            if ((cs >= 15 && cs <= 29) || cs == 43) continue;   /* IP/UDP (Task 8), global info */
            r = pbt_simple(&x, cs, sp);
            if (cs == 30) T_EQ(r, invalidLength);
            else if ((cs >= 31 && cs <= 42) || cs == 50) T_EQ(r, invalidStreamPtr);
            else if (cs == 1 || cs == 9 || cs == 10 || cs == 11 || cs == 14 || cs == 44) T_EQ(r, 0);
            else T_EQ(r, MT_PB_UNIMP);                    /* the -4 row */
        }
    }
}

static void global_info_tables(void) {
    pbt x;
    TCPiopb *pb;
    StreamPtr s;
    TCPParam *p;
    MacPtr32 *cdb;
    int i, found = 0;
    pbt_init(&x, 1);
    s = pbt_create(&x, 4096, 0, 0);
    pb = pbt_pb(TCPGlobalInfo, 0);
    T_EQ(pbt_call(&x, pb), 0);                            /* matrix 800 */
    p = (TCPParam *)pbf_p(pb->csParam.globalInfo.tcpParamPtr);
    T_CHECK(p != NULL && pbf_p(pb->csParam.globalInfo.tcpStatsPtr) != NULL);   /* matrix 800: ours */
    if (p) {
        T_EQ(p->tcpMaxSegSize, 1460);                     /* matrix 800 */
        T_EQ(p->tcpMaxWindow, 65535);
    }
    T_EQ(pb->csParam.globalInfo.maxTCPConnections, 0);    /* matrix 800: stays 0 */
    cdb = (MacPtr32 *)pbf_p(pb->csParam.globalInfo.tcpCDBTable[0]);
    for (i = 0; cdb && i < MT_PB_MAX_TCP; i++) found += cdb[i] == s;
    T_EQ(found, 1);
}

/* TCPClose while the glue still holds TCPSend PBs the core has no room for: the FIN goes
 * after their bytes, never before (they would be lost), and every PB completes 0. */
static void close_after_queued_sends_fin_follows_data(void) {
    pbt x;
    pbt_conn c;
    StreamPtr s;
    TCPiopb *snd[4], *cl;
    tcpv v;
    int i, fin_seen = 0, f;
    uint32_t total = 0;
    pbt_init(&x, 1);
    s = pbt_est(&x, pbf_asr(asr_nop), 0, &c);
    for (i = 0; i < 4; i++) { snd[i] = pbt_send(&x, s, "0123456789", 10, 1); total += 10; }
    fake_clear(&x.fl);
    cl = pbt_pb(TCPClose, s);
    T_EQ(pbt_call(&x, cl), inProgress);
    T_EQ(pbt_send(&x, s, "z", 1, 1)->ioResult, connectionClosing);   /* matrix 408 */
    for (f = 0; f < x.fl.n; f++)
        if (view_tcp(x.fl.f[f], x.fl.len[f], &v) && (v.flags & TF_FIN)) fin_seen = 1;
    T_EQ(fin_seen, 0);                                    /* PBs 3 and 4 are not in the core yet */
    fake_clear(&x.fl);
    pbt_peer(&x, c.lport, c.ps, c.ms + 20, TF_ACK, 8192, NULL, 0);
    T_EQ(snd[1]->ioResult, 0);
    for (f = 0; f < x.fl.n; f++)
        if (view_tcp(x.fl.f[f], x.fl.len[f], &v) && (v.flags & TF_FIN)) {
            fin_seen = 1;
            T_EQ(v.seq + v.dlen, c.ms + total);           /* the FIN follows the last PB byte */
        }
    T_EQ(fin_seen, 1);
    pbt_peer(&x, c.lport, c.ps, c.ms + total + 1, TF_ACK, 8192, NULL, 0);
    for (i = 0; i < 4; i++) T_EQ(snd[i]->ioResult, 0);
    T_EQ(cl->ioResult, 0);
}

T_MAIN(close_with_pending_rcv_completes_rcv, peer_close_closing_after_data_read,
       abort_connected_pending_rcv_order, release_with_everything_pending_completes_all_pbs,
       call_from_inside_asr_ok, second_rcv_queued_per_matrix, status_fields, passive_open_accepts,
       passive_open_port0_per_matrix, unconfigured_returns_matrix_error,
       zero_len_and_odd_wds_entries, code_tables_one_row_each,
       remote_reset_and_ulp_timeout_reasons, cscode_sweep_bad_stream, global_info_tables,
       close_after_queued_sends_fin_follows_data)
