#include "t.h"
#include "net.h"
#include "mt_int.h"
#include "mt_util.h"

static const uint8_t MSS1460[4] = {2, 4, 0x05, 0xB4};

static mt_stack *setup(fake_link *fl) {
    mt_stack *s = tstack_new(fl, T_ME);
    mt_arp_insert(s, T_PEER, T_PEER_MAC, 0);
    return s;
}

/* Inject a segment from `src` to us. The frame always sits at an odd address. */
static void inject_from(mt_stack *s, mt_ip4 src, uint16_t sport, uint16_t dport, uint32_t seq,
                        uint32_t ack, uint8_t flags, const uint8_t *opt, uint8_t optlen,
                        const uint8_t *data, uint16_t dlen, uint32_t now) {
    static uint8_t buf[1700];
    uint8_t *f = buf + 1;
    uint16_t n = mk_tcp(f, src, sport, T_ME, dport, seq, ack, flags, 8192, opt, optlen, data,
                        dlen);
    mt_input(s, f, n, now);
}

static void inject(mt_stack *s, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                   uint8_t flags, const uint8_t *data, uint16_t dlen, uint32_t now) {
    inject_from(s, T_PEER, sport, dport, seq, ack, flags,
                (flags & TF_SYN) ? MSS1460 : NULL, (flags & TF_SYN) ? 4 : 0, data, dlen, now);
}

static int last_tcp(const fake_link *fl, tcpv *v) {
    return fl->n > 0 && view_tcp(fl->f[fl->n - 1], fl->len[fl->n - 1], v);
}

static void test_active_open(void) {
    fake_link fl; rec r; mt_tcp_info in; tcpv v;
    mt_stack *s = setup(&fl);
    static uint8_t b[8193];
    mt_tcp *t = mt_tcp_create(s, b + 1, 8192, rec_cb, &r);   /* odd app buffer */
    uint32_t iss = 1u + 100u * 250u;
    memset(&r, 0, sizeof r);
    T_CHECK(t != NULL);
    T_EQ(mt_tcp_open(t, T_PEER, 23, 0, 100), MT_OK);
    T_EQ(fl.n, 1);
    T_CHECK(last_tcp(&fl, &v));
    T_EQ(v.flags, TF_SYN); T_EQ(v.mss, 1460); T_CHECK(v.sport >= 49152); T_EQ(v.dport, 23);
    T_EQ(v.seq, iss); T_CHECK(v.checksum_ok); T_EQ(v.dst, T_PEER);
    mt_tcp_get_info(t, &in);
    T_EQ(in.state, MT_TCP_SYN_SENT);
    inject(s, 23, v.sport, 9000, iss + 1, TF_SYN | TF_ACK, NULL, 0, 105);
    T_EQ(fl.n, 2);
    T_CHECK(last_tcp(&fl, &v));
    T_EQ(v.flags, TF_ACK); T_EQ(v.seq, iss + 1); T_EQ(v.ack, 9001); T_CHECK(v.checksum_ok);
    T_EQ(r.n, 1); T_EQ(r.e[0].ev, MT_TCP_EV_ESTABLISHED);
    mt_tcp_get_info(t, &in);
    T_EQ(in.state, MT_TCP_ESTABLISHED); T_EQ(in.snd_wnd, 8192); T_EQ(in.rcv_wnd, 8192);   /* whole buffer */
    T_EQ(in.rip, T_PEER); T_EQ(in.lip, T_ME);
    T_EQ(mt_get_stats(s)->tcp_active_opens, 1); T_EQ(mt_get_stats(s)->tcp_conns_open, 1);
    mt_tick(s, 100000);
    T_EQ(fl.n, 2);                                  /* SYN timer is off once established */
}

static void test_open_refused(void) {
    fake_link fl; rec r; tcpv v; mt_tcp_info in;
    mt_stack *s = setup(&fl);
    static uint8_t b[8192];
    mt_tcp *t = mt_tcp_create(s, b, sizeof b, rec_cb, &r);
    memset(&r, 0, sizeof r);
    T_EQ(mt_tcp_open(t, T_PEER, 23, 0, 0), MT_OK);
    T_CHECK(last_tcp(&fl, &v));
    inject(s, 23, v.sport, 0, v.seq + 1, TF_RST | TF_ACK, NULL, 0, 3);
    T_EQ(r.n, 1); T_EQ(r.e[0].ev, MT_TCP_EV_TERMINATE); T_EQ(r.e[0].arg, MT_TERM_OPEN_FAILED);
    mt_tcp_get_info(t, &in);
    T_EQ(in.state, MT_TCP_CLOSED);
    T_EQ(fl.n, 1);
    mt_tick(s, 60000);
    T_EQ(fl.n, 1); T_EQ(r.n, 1);
}

static void test_open_syn_retry_then_fail(void) {
    fake_link fl; rec r; tcpv v;
    mt_stack *s = setup(&fl);
    static uint8_t b[8192];
    mt_tcp *t = mt_tcp_create(s, b, sizeof b, rec_cb, &r);
    uint32_t iss;
    memset(&r, 0, sizeof r);
    mt_tcp_set_ulp_timeout(t, 10, 1);
    T_EQ(mt_tcp_open(t, T_PEER, 23, 0, 0), MT_OK);
    T_CHECK(last_tcp(&fl, &v));
    iss = v.seq;
    T_EQ(mt_next_deadline(s, 0), 1000);
    mt_tick(s, 999);   T_EQ(fl.n, 1);
    mt_tick(s, 1000);  T_EQ(fl.n, 2);
    mt_tick(s, 2999);  T_EQ(fl.n, 2);
    T_EQ(mt_next_deadline(s, 2999), 1);
    mt_tick(s, 3000);  T_EQ(fl.n, 3);
    mt_tick(s, 6999);  T_EQ(fl.n, 3);
    mt_tick(s, 7000);  T_EQ(fl.n, 4);
    T_CHECK(last_tcp(&fl, &v)); T_EQ(v.flags, TF_SYN); T_EQ(v.seq, iss); T_EQ(v.mss, 1460);
    T_EQ(mt_get_stats(s)->tcp_rexmt, 3);
    T_EQ(r.n, 0);
    T_EQ(mt_next_deadline(s, 7000), 3000);          /* ULP timeout, not the 8 s RTO */
    mt_tick(s, 10000);
    T_EQ(r.n, 1); T_EQ(r.e[0].ev, MT_TCP_EV_TERMINATE); T_EQ(r.e[0].arg, MT_TERM_OPEN_FAILED);
    T_EQ(fl.n, 4);
    T_CHECK(mt_next_deadline(s, 10000) > 500000);   /* only the ARP entry ages now */
}

static void test_passive_open(void) {
    fake_link fl; rec r; tcpv v; mt_tcp_info in;
    mt_stack *s = setup(&fl);
    static uint8_t b[8192];
    mt_tcp *t = mt_tcp_create(s, b, sizeof b, rec_cb, &r);
    uint32_t iss = 1u + 50u * 250u;
    memset(&r, 0, sizeof r);
    T_EQ(mt_tcp_listen(t, 0, 0, 21), MT_OK);
    mt_tcp_get_info(t, &in);
    T_EQ(in.state, MT_TCP_LISTEN);
    inject(s, 1025, 21, 7000, 0, TF_SYN, NULL, 0, 50);
    T_EQ(fl.n, 1);
    T_CHECK(last_tcp(&fl, &v));
    T_EQ(v.flags, TF_SYN | TF_ACK); T_EQ(v.seq, iss); T_EQ(v.ack, 7001); T_EQ(v.mss, 1460);
    T_EQ(v.sport, 21); T_EQ(v.dport, 1025); T_CHECK(v.checksum_ok);
    mt_tcp_get_info(t, &in);
    T_EQ(in.state, MT_TCP_SYN_RCVD); T_EQ(in.rport, 1025);
    T_EQ(r.n, 0);
    inject(s, 1025, 21, 7001, iss + 1, TF_ACK, NULL, 0, 60);
    T_EQ(r.n, 1); T_EQ(r.e[0].ev, MT_TCP_EV_ESTABLISHED);
    mt_tcp_get_info(t, &in);
    T_EQ(in.state, MT_TCP_ESTABLISHED);
    T_EQ(fl.n, 1);
    fake_clear(&fl);
    inject(s, 1026, 21, 100, 0, TF_SYN, NULL, 0, 70);   /* the listener is spoken for: RST */
    T_EQ(fl.n, 1);
    T_CHECK(last_tcp(&fl, &v));
    T_EQ(v.flags, TF_RST | TF_ACK); T_EQ(v.ack, 101); T_EQ(v.seq, 0);
    T_EQ(v.sport, 21); T_EQ(v.dport, 1026);
    T_EQ(mt_get_stats(s)->tcp_passive_opens, 1); T_EQ(mt_get_stats(s)->tcp_resets_out, 1);
}

static void test_listen_filter(void) {
    fake_link fl; rec r; tcpv v;
    mt_stack *s = setup(&fl);
    static uint8_t b[8192], b2[8192];
    mt_tcp *t = mt_tcp_create(s, b, sizeof b, rec_cb, &r);
    mt_tcp *u = mt_tcp_create(s, b2, sizeof b2, rec_cb, &r);
    memset(&r, 0, sizeof r);
    mt_arp_insert(s, T_PEER + 1, T_PEER_MAC, 0);
    T_EQ(mt_tcp_listen(t, T_PEER, 0, 21), MT_OK);
    inject_from(s, T_PEER + 1, 1025, 21, 5, 0, TF_SYN, MSS1460, 4, NULL, 0, 1);
    T_EQ(fl.n, 1);
    T_CHECK(last_tcp(&fl, &v));
    T_CHECK(v.flags & TF_RST); T_EQ(v.dst, T_PEER + 1);
    fake_clear(&fl);
    /* rport filter and lport wildcard; the more specific listener wins */
    T_EQ(mt_tcp_listen(u, 0, 0, 0), MT_OK);        /* any peer, any port */
    inject_from(s, T_PEER, 2000, 21, 5, 0, TF_SYN, MSS1460, 4, NULL, 0, 2);
    T_EQ(fl.n, 1);
    T_CHECK(last_tcp(&fl, &v)); T_EQ(v.flags, TF_SYN | TF_ACK);
    { mt_tcp_info a, c; mt_tcp_get_info(t, &a); mt_tcp_get_info(u, &c);
      T_EQ(a.state, MT_TCP_SYN_RCVD); T_EQ(c.state, MT_TCP_LISTEN); }
    /* a reset of the half-open connection re-arms the listener */
    inject(s, 2000, 21, 6, 0, TF_RST, NULL, 0, 3);
    { mt_tcp_info a; mt_tcp_get_info(t, &a);
      T_EQ(a.state, MT_TCP_LISTEN); T_EQ(a.rip, 0); T_EQ(a.lport, 21); }
    T_EQ(r.n, 0);
}

static void test_graceful_close_active(void) {
    fake_link fl; rec r; tcpv v; mt_tcp_info in; uint32_t ps, ms;
    mt_stack *s = setup(&fl);
    mt_tcp *t;
    memset(&r, 0, sizeof r);
    t = tconn_established(s, &fl, &r, &ps, &ms);
    T_CHECK(t != NULL);
    mt_tcp_get_info(t, &in);
    T_EQ(mt_tcp_close(t, 100), MT_OK);
    T_EQ(fl.n, 1);
    T_CHECK(last_tcp(&fl, &v));
    T_EQ(v.flags, TF_FIN | TF_ACK); T_EQ(v.seq, ms); T_EQ(v.ack, ps);
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_FIN_WAIT_1);
    T_EQ(mt_tcp_close(t, 100), MT_ERR_STATE);
    inject(s, 23, in.lport, ps, ms + 1, TF_ACK, NULL, 0, 110);
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_FIN_WAIT_2);
    T_EQ(fl.n, 1);
    inject(s, 23, in.lport, ps, ms + 1, TF_FIN | TF_ACK, NULL, 0, 120);
    T_EQ(fl.n, 2);
    T_CHECK(last_tcp(&fl, &v)); T_EQ(v.flags, TF_ACK); T_EQ(v.ack, ps + 1); T_EQ(v.seq, ms + 1);
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_TIME_WAIT);
    T_EQ(r.n, 2); T_EQ(r.e[1].ev, MT_TCP_EV_CLOSING);
    T_EQ(mt_next_deadline(s, 120), 2000);
    mt_tick(s, 2119);
    T_EQ(r.n, 2);
    mt_tick(s, 2120);
    T_EQ(r.n, 3); T_EQ(r.e[2].ev, MT_TCP_EV_TERMINATE); T_EQ(r.e[2].arg, MT_TERM_ULP_CLOSE);
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_CLOSED);
    T_EQ(mt_get_stats(s)->tcp_conns_open, 0);
}

static void test_fin_with_unread_data(void) {
    fake_link fl; rec r; tcpv v; mt_tcp_info in; uint32_t ps, ms;
    mt_stack *s = setup(&fl);
    mt_tcp *t;
    memset(&r, 0, sizeof r);
    t = tconn_established(s, &fl, &r, &ps, &ms);
    mt_tcp_get_info(t, &in);
    inject(s, 23, in.lport, ps, ms, TF_PSH | TF_ACK | TF_FIN, (const uint8_t *)"login: ", 7, 10);
    T_EQ(fl.n, 1);                                  /* the ACK left before mt_input returned */
    T_CHECK(last_tcp(&fl, &v)); T_EQ(v.flags, TF_ACK); T_EQ(v.ack, ps + 8);
    T_EQ(r.n, 3);
    T_EQ(r.e[1].ev, MT_TCP_EV_DATA); T_EQ(r.e[1].arg, 7);
    T_EQ(r.e[2].ev, MT_TCP_EV_CLOSING);
    mt_tcp_get_info(t, &in);
    T_EQ(in.state, MT_TCP_CLOSE_WAIT); T_EQ(in.rcv_avail, 7);
    T_EQ(mt_tcp_close(t, 20), MT_OK);
    T_CHECK(last_tcp(&fl, &v));
    T_EQ(fl.n, 2); T_EQ(v.flags, TF_FIN | TF_ACK); T_EQ(v.ack, ps + 8);
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_LAST_ACK); T_EQ(in.rcv_avail, 7);
    inject(s, 23, in.lport, ps + 8, ms + 1, TF_ACK, NULL, 0, 30);
    T_EQ(r.n, 4); T_EQ(r.e[3].ev, MT_TCP_EV_TERMINATE); T_EQ(r.e[3].arg, MT_TERM_ULP_CLOSE);
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_CLOSED);
}

static void test_remote_reset(void) {
    fake_link fl; rec r; tcpv v; mt_tcp_info in; uint32_t ps, ms;
    mt_stack *s = setup(&fl);
    mt_tcp *t;
    memset(&r, 0, sizeof r);
    t = tconn_established(s, &fl, &r, &ps, &ms);
    mt_tcp_get_info(t, &in);
    inject(s, 23, in.lport, ps + 100000u, 0, TF_RST, NULL, 0, 10);   /* out of window */
    T_EQ(r.n, 1);
    T_EQ(fl.n, 1);
    T_CHECK(last_tcp(&fl, &v)); T_EQ(v.flags, TF_ACK); T_EQ(v.seq, ms); T_EQ(v.ack, ps);
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_ESTABLISHED);
    inject(s, 23, in.lport, ps - 5, 0, TF_RST, NULL, 0, 11);         /* old */
    T_EQ(fl.n, 2); T_EQ(r.n, 1);
    inject(s, 23, in.lport, ps + 10, 0, TF_RST, NULL, 0, 12);        /* in window */
    T_EQ(r.n, 2); T_EQ(r.e[1].ev, MT_TCP_EV_TERMINATE); T_EQ(r.e[1].arg, MT_TERM_REMOTE_ABORT);
    T_EQ(fl.n, 2);
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_CLOSED);
    T_EQ(mt_get_stats(s)->tcp_resets_in, 1);
}

static void test_abort_sends_rst(void) {
    fake_link fl; rec r; tcpv v; mt_tcp_info in; uint32_t ps, ms;
    mt_stack *s = setup(&fl);
    mt_tcp *t;
    memset(&r, 0, sizeof r);
    t = tconn_established(s, &fl, &r, &ps, &ms);
    mt_tcp_abort(t, 10);
    T_EQ(fl.n, 1);
    T_CHECK(last_tcp(&fl, &v)); T_CHECK(v.flags & TF_RST); T_EQ(v.seq, ms); T_CHECK(v.checksum_ok);
    T_EQ(r.n, 2); T_EQ(r.e[1].ev, MT_TCP_EV_TERMINATE); T_EQ(r.e[1].arg, MT_TERM_ULP_ABORT);
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_CLOSED);
    mt_tcp_abort(t, 11);                            /* TERMINATE exactly once */
    T_EQ(r.n, 2); T_EQ(fl.n, 1);
}

static void test_release_no_callbacks(void) {
    fake_link fl; rec r; tcpv v; mt_tcp_info in; uint32_t ps, ms;
    mt_stack *s = setup(&fl);
    mt_tcp *t;
    memset(&r, 0, sizeof r);
    t = tconn_established(s, &fl, &r, &ps, &ms);
    mt_tcp_get_info(t, &in);
    mt_tcp_release(t);
    T_EQ(fl.n, 1);
    T_CHECK(last_tcp(&fl, &v)); T_CHECK(v.flags & TF_RST); T_EQ(v.seq, ms);
    T_EQ(r.n, 1);                                   /* only the ESTABLISHED from before */
    inject(s, 23, in.lport, ps, ms, TF_ACK, (const uint8_t *)"zz", 2, 20);
    mt_tick(s, 500000);
    T_EQ(r.n, 1);
    T_EQ(fl.n, 2);                                  /* the late data draws a RST, nothing else */
    T_EQ(mt_get_stats(s)->tcp_conns_open, 0);
}

static void test_tcb_exhaustion(void) {
    fake_link fl; rec r;
    mt_stack *s = setup(&fl);
    static uint8_t bufs[17][4096];
    mt_tcp *t[17];
    int i;
    memset(&r, 0, sizeof r);
    for (i = 0; i < 16; i++) { t[i] = mt_tcp_create(s, bufs[i], 4096, rec_cb, &r); T_CHECK(t[i] != NULL); }
    T_CHECK(mt_tcp_create(s, bufs[16], 4096, rec_cb, &r) == NULL);
    T_CHECK(mt_tcp_create(s, bufs[16], 511, rec_cb, &r) == NULL);    /* too small anyway */
    mt_tcp_release(t[5]);
    t[16] = mt_tcp_create(s, bufs[16], 4096, rec_cb, &r);
    T_CHECK(t[16] != NULL);
    T_EQ(fl.n, 0);                                  /* releasing a never-opened TCB is silent */
}

static void test_tcp_bad_options(void) {
    fake_link fl; rec r; tcpv v; mt_tcp_info in;
    mt_stack *s = setup(&fl);
    static uint8_t b[8192];
    mt_tcp *t = mt_tcp_create(s, b, sizeof b, rec_cb, &r);
    static const uint8_t zero_len[4] = {2, 0, 0, 0}, past_end[4] = {2, 8, 5, 0xB4},
                         one_len[4] = {3, 1, 0, 0}, trunc[4] = {1, 1, 1, 2},
                         eol[4] = {0, 99, 99, 99};
    memset(&r, 0, sizeof r);
    T_EQ(mt_tcp_open(t, T_PEER, 23, 0, 0), MT_OK);
    T_CHECK(last_tcp(&fl, &v));
    inject_from(s, T_PEER, 23, v.sport, 9000, v.seq + 1, TF_SYN | TF_ACK, zero_len, 4, NULL, 0, 1);
    T_EQ(mt_get_stats(s)->tcp_in_drops, 1);
    inject_from(s, T_PEER, 23, v.sport, 9000, v.seq + 1, TF_SYN | TF_ACK, past_end, 4, NULL, 0, 1);
    T_EQ(mt_get_stats(s)->tcp_in_drops, 2);
    inject_from(s, T_PEER, 23, v.sport, 9000, v.seq + 1, TF_SYN | TF_ACK, one_len, 4, NULL, 0, 1);
    T_EQ(mt_get_stats(s)->tcp_in_drops, 3);
    inject_from(s, T_PEER, 23, v.sport, 9000, v.seq + 1, TF_SYN | TF_ACK, trunc, 4, NULL, 0, 1);
    T_EQ(mt_get_stats(s)->tcp_in_drops, 4);         /* kind 2 at the last byte: no room for len */
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_SYN_SENT);
    T_EQ(fl.n, 1); T_EQ(r.n, 0);
    /* EOL stops the parse (garbage after it is not looked at); NOPs are skipped */
    inject_from(s, T_PEER, 23, v.sport, 9000, v.seq + 1, TF_SYN | TF_ACK, eol, 4, NULL, 0, 1);
    T_EQ(mt_get_stats(s)->tcp_in_drops, 4);
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_ESTABLISHED);   /* no MSS: default 536 */
    T_EQ(r.n, 1);
}

static void test_tcp_bad_checksum_dropped(void) {
    fake_link fl; rec r; tcpv v; mt_tcp_info in;
    mt_stack *s = setup(&fl);
    static uint8_t b[8192], f[200];
    mt_tcp *t = mt_tcp_create(s, b, sizeof b, rec_cb, &r);
    uint16_t n;
    memset(&r, 0, sizeof r);
    T_EQ(mt_tcp_open(t, T_PEER, 23, 0, 0), MT_OK);
    T_CHECK(last_tcp(&fl, &v));
    n = mk_tcp(f, T_PEER, 23, T_ME, v.sport, 9000, v.seq + 1, TF_SYN | TF_ACK, 8192, MSS1460, 4,
               NULL, 0);
    f[14 + 20 + 17] ^= 0x40;
    mt_input(s, f, n, 1);
    T_EQ(mt_get_stats(s)->tcp_in_drops, 1);
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_SYN_SENT);
    T_EQ(r.n, 0); T_EQ(fl.n, 1);
    f[14 + 20 + 17] ^= 0x40;                        /* repaired: accepted */
    mt_input(s, f, n, 1);
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_ESTABLISHED);
    T_EQ(mt_get_stats(s)->tcp_in, 1);
}

static void test_icmp_unreach_event(void) {
    fake_link fl; rec r; mt_tcp_info in; uint32_t ps, ms;
    mt_stack *s = setup(&fl);
    mt_tcp *t;
    uint8_t q[80], m[40], f[200];
    uint16_t qn, fn;
    memset(&r, 0, sizeof r);
    t = tconn_established(s, &fl, &r, &ps, &ms);
    mt_tcp_get_info(t, &in);
    qn = mk_tcp(q, T_ME, in.lport, T_PEER, 23, ms, ps, TF_ACK, 100, NULL, 0, NULL, 0);
    (void)qn;
    memset(m, 0, sizeof m);
    m[0] = 3; m[1] = 3;
    memcpy(m + 8, q + 14, 28);                      /* original IP header + 8 bytes */
    mt_wr16(m + 2, mt_cksum_fold(mt_cksum_add(0, m, 36)));
    fn = mk_ip(f, T_PEER, T_ME, 1, 5, 0, m, 36);
    mt_input(s, f, fn, 10);
    T_EQ(r.n, 2); T_EQ(r.e[1].ev, MT_TCP_EV_ICMP); T_EQ(r.e[1].arg, 0x0303);
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_ESTABLISHED);    /* reported, not aborted */
    T_EQ(fl.n, 0);
}

static void test_arp_hazard_no_duplicate(void) {
    fake_link fl; rec r; tcpv v; arpv a; uint8_t f[64]; uint16_t n;
    mt_stack *s = tstack_new(&fl, T_ME);            /* peer NOT resolved */
    static uint8_t b[8192];
    mt_tcp *t = mt_tcp_create(s, b, sizeof b, rec_cb, &r);
    uint32_t ps, ms;
    int i, syns = 0;
    mt_tcp_info in;
    memset(&r, 0, sizeof r);
    /* (a) the held SYN is released by the ARP reply and must not be sent twice */
    T_EQ(mt_tcp_open(t, T_PEER, 23, 0, 0), MT_OK);
    T_EQ(fl.n, 1);
    T_CHECK(view_arp(fl.f[0], fl.len[0], &a)); T_EQ(a.op, 1);
    n = mk_arp(f, 2, T_PEER_MAC, T_PEER, T_ME_MAC, T_ME);
    mt_input(s, f, n, 5);
    for (i = 1; i < fl.n; i++) if (view_tcp(fl.f[i], fl.len[i], &v) && v.flags == TF_SYN) syns++;
    T_EQ(fl.n, 2); T_EQ(syns, 1); T_EQ(mt_get_stats(s)->tcp_rexmt, 0);

    /* (b) established, ARP expired: data segment held and released -> exactly one copy */
    {
        fake_link fl2; rec r2; mt_stack *s2; mt_tcp *t2;
        memset(&r2, 0, sizeof r2);
        s2 = tstack_new(&fl2, T_ME);
        t2 = tconn_established(s2, &fl2, &r2, &ps, &ms);
        mt_tcp_get_info(t2, &in);
        mt_tick(s2, 700000);                        /* the ARP entry has aged out */
        T_EQ(tsend(t2, (const uint8_t *)"AAAA", 4, 0, 0, 700000), 4);
        T_EQ(fl2.n, 1);                             /* ARP request; segment held */
        T_CHECK(view_arp(fl2.f[0], fl2.len[0], &a));
        n = mk_arp(f, 2, T_PEER_MAC, T_PEER, T_ME_MAC, T_ME);
        mt_input(s2, f, n, 700001);
        T_EQ(fl2.n, 2);
        T_CHECK(view_tcp(fl2.f[1], fl2.len[1], &v)); T_EQ(v.seq, ms); T_EQ(v.dlen, 4);
        T_EQ(mt_get_stats(s2)->tcp_rexmt, 0);
        /* (c) ack it, age ARP out again; B then C: C replaces B in the held slot. The reply
         * releases C, and B (first unacked, NOT the released frame) is retransmitted. */
        inject(s2, 23, in.lport, ps, ms + 4, TF_ACK, NULL, 0, 700010);
        fake_clear(&fl2);
        mt_tick(s2, 1400000);
        T_EQ(tsend(t2, (const uint8_t *)"BBBB", 4, 0, 0, 1400000), 4);
        T_EQ(tsend(t2, (const uint8_t *)"CCCC", 4, 1, 0, 1400000), 4);   /* push: else Nagle holds C */
        T_EQ(fl2.n, 1);                             /* just the ARP request */
        n = mk_arp(f, 2, T_PEER_MAC, T_PEER, T_ME_MAC, T_ME);
        mt_input(s2, f, n, 1400002);
        T_EQ(fl2.n, 3);
        T_CHECK(view_tcp(fl2.f[1], fl2.len[1], &v)); T_EQ(v.seq, ms + 8); T_MEMEQ(v.data, "CCCC", 4);
        T_CHECK(view_tcp(fl2.f[2], fl2.len[2], &v)); T_EQ(v.seq, ms + 4); T_MEMEQ(v.data, "BBBB", 4);
        T_EQ(mt_get_stats(s2)->tcp_rexmt, 1);
    }
}

/* One segment built from two caller segments: the first run is 5 or 6 bytes (odd or even
 * boundary inside the payload), the source address odd or even, total odd or even. */
static void test_data_segment_split_sources(void) {
    static const uint16_t first[4] = {5, 6, 5, 6};
    static const uint16_t lens[4] = {1000, 1000, 999, 999};
    int k;
    for (k = 0; k < 4; k++) {
        fake_link fl; rec r; tcpv v; uint32_t ps, ms; static uint8_t mem[1002]; uint16_t i;
        mt_stack *s = tstack_new(&fl, T_ME);
        mt_tcp *t;
        uint8_t *d = mem + (k & 1);                 /* odd source address on alternate cases */
        static mt_seg sg[2];
        memset(&r, 0, sizeof r);
        t = tconn_established(s, &fl, &r, &ps, &ms);
        for (i = 0; i < 1000; i++) d[i] = (uint8_t)(i * 5 + 1);
        sg[0].p = d; sg[0].len = first[k];
        sg[1].p = d + first[k]; sg[1].len = (uint16_t)(lens[k] - first[k]);
        T_EQ(mt_tcp_send_segs(t, sg, 2, 0, 0, NULL, NULL, 10), MT_OK);
        T_EQ(fl.n, 1);
        T_CHECK(last_tcp(&fl, &v));
        T_CHECK(v.checksum_ok); T_EQ(v.dlen, lens[k]); T_EQ(v.seq, ms);
        T_EQ(v.flags, TF_ACK | TF_PSH);
        T_MEMEQ(v.data, d, lens[k]);
    }
}

T_MAIN(test_active_open, test_open_refused, test_open_syn_retry_then_fail, test_passive_open,
       test_listen_filter, test_graceful_close_active, test_fin_with_unread_data,
       test_remote_reset, test_abort_sends_rst, test_release_no_callbacks, test_tcb_exhaustion,
       test_tcp_bad_options, test_tcp_bad_checksum_dropped, test_icmp_unreach_event,
       test_arp_hazard_no_duplicate, test_data_segment_split_sources)
