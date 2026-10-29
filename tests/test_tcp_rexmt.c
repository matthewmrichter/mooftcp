#include "t.h"
#include "net.h"
#include "mt_int.h"
#include "mt_util.h"

/* TCP reliability: RTO, RTT estimation (Karn), congestion control, fast retransmit, go-back-N
 * recovery, persist, ULP timeout, FIN_WAIT_2 timeout, ARP kick, mt_next_deadline. Each test
 * establishes its own connection at now=0 (peer MSS 1460, our rings buflen/4 : rest). */

static const uint8_t MSS1460[4] = {2, 4, 0x05, 0xB4};
static uint16_t g_pwin = 8192;
#define MSS 1460u

typedef struct { mt_stack *s; fake_link *fl; mt_tcp *t; uint16_t lport; uint32_t ps, ms; } C;

static void inject(C *c, uint32_t ack, uint8_t flags, uint16_t win, uint32_t now) {
    static uint8_t buf[200];
    uint8_t *f = buf + 1;                                   /* odd frame address */
    uint16_t len = mk_tcp(f, T_PEER, 23, T_ME, c->lport, c->ps, ack, flags, win, NULL, 0, NULL, 0);
    mt_input(c->s, f, len, now);
}
static void ack_to(C *c, uint32_t ack, uint32_t now) { inject(c, ack, TF_ACK, g_pwin, now); }

static int open_conn(C *c, fake_link *fl, uint8_t *buf, uint32_t buflen, mt_tcp_cb cb, void *user) {
    tcpv v;
    uint8_t f[200];
    uint16_t n;
    c->fl = fl;
    c->s = tstack_new(fl, T_ME);
    mt_arp_insert(c->s, T_PEER, T_PEER_MAC, 0);
    c->t = mt_tcp_create(c->s, buf, buflen, cb, user);
    if (!c->t || mt_tcp_open(c->t, T_PEER, 23, 0, 0) != MT_OK) return 0;
    if (fl->n != 1 || !view_tcp(fl->f[0], fl->len[0], &v)) return 0;
    c->lport = v.sport;
    n = mk_tcp(f, T_PEER, 23, T_ME, v.sport, 5000, v.seq + 1, TF_SYN | TF_ACK, g_pwin,
               MSS1460, 4, NULL, 0);
    mt_input(c->s, f, n, 0);
    c->ps = 5001;
    c->ms = v.seq + 1;
    fake_clear(fl);
    return 1;
}

#define SETUP(buflen, win, cb, user) \
    static fake_link fl; static uint8_t buf[100000]; C c; \
    memset(&c, 0, sizeof c); g_pwin = (win); \
    T_CHECK(open_conn(&c, &fl, buf + 1, (buflen), (cb), (user)))

static int tcp_at(const fake_link *fl, int i, tcpv *v) { return view_tcp(fl->f[i], fl->len[i], v); }
static int count_ev(const rec *r, int ev) {
    int i, n = 0;
    for (i = 0; i < r->n && i < 32; i++) n += r->e[i].ev == ev;
    return n;
}
static void pat(uint8_t *p, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) p[i] = (uint8_t)(i * 5u + 1u);
}
/* Advance the clock to the stack's next deadline; returns the new now. */
static uint32_t step(C *c, uint32_t now) {
    now += mt_next_deadline(c->s, now);
    mt_tick(c->s, now);
    return now;
}
static int data_frames(const fake_link *fl, uint32_t *seqs, int max) {
    int i, n = 0;
    tcpv v;
    for (i = 0; i < fl->n; i++)
        if (tcp_at(fl, i, &v) && v.dlen && n < max) seqs[n++] = v.seq;
    return n;
}
static void forget_arp(mt_stack *s) {
    uint16_t i;
    for (i = 0; i < s->lim.arp_entries; i++) s->arp[i].state = MT_ARP_FREE;
}

static void test_rto_retransmit(void) {
    static uint8_t d[100];
    tcpv v, v0;
    pat(d, 100);
    {
        SETUP(8192, 8192, NULL, NULL);
        T_EQ(mt_tcp_send(c.t, d, 100, 1, 0, 10), 100);
        T_EQ(fl.n, 1);
        T_CHECK(tcp_at(&fl, 0, &v0));
        T_EQ(mt_next_deadline(c.s, 10), 1000);
        mt_tick(c.s, 1009); T_EQ(fl.n, 1);
        mt_tick(c.s, 1010); T_EQ(fl.n, 2);
        T_CHECK(tcp_at(&fl, 1, &v)); T_EQ(v.seq, v0.seq); T_EQ(v.dlen, 100);
        T_MEMEQ(v.data, d, 100);
        T_EQ(mt_next_deadline(c.s, 1010), 2000);                 /* backed off */
        mt_tick(c.s, 3009); T_EQ(fl.n, 2);
        mt_tick(c.s, 3010); T_EQ(fl.n, 3);
        T_EQ(mt_next_deadline(c.s, 3010), 4000);
        ack_to(&c, c.ms + 100, 3020);
        mt_tick(c.s, 100000);
        T_EQ(fl.n, 3);
        T_EQ(mt_get_stats(c.s)->tcp_rexmt, 2);
    }
}

static void test_rtt_estimate(void) {
    static uint8_t d[100];
    mt_tcp_info in;
    {
        SETUP(8192, 8192, NULL, NULL);
        mt_tcp_send(c.t, d, 100, 1, 0, 100);
        ack_to(&c, c.ms + 100, 150);
        mt_tcp_get_info(c.t, &in);
        T_EQ(in.srtt_ms, 50);
        T_EQ(in.rto_ms, 200);                                    /* srtt + 4*rttvar = 150, clamped */
        mt_tcp_send(c.t, d, 100, 1, 0, 1000);                    /* second sample: 150 ms */
        ack_to(&c, c.ms + 200, 1150);
        mt_tcp_get_info(c.t, &in);
        T_EQ(in.srtt_ms, 62);                                    /* 50 + (150 - 50)/8 = 62.5 */
        T_EQ(in.rto_ms, 62 + 175);                               /* rttvar = 3/4*25 + 100/4 = 43.75 */
    }
}

static void test_karn(void) {
    static uint8_t d[100];
    mt_tcp_info in;
    {
        SETUP(8192, 8192, NULL, NULL);
        mt_tcp_send(c.t, d, 100, 1, 0, 100);
        ack_to(&c, c.ms + 100, 150);                             /* srtt 50 */
        mt_tcp_send(c.t, d, 100, 1, 0, 1000);
        mt_tick(c.s, 1200);                                      /* RTO 200: retransmitted */
        T_EQ(fl.n, 3);
        ack_to(&c, c.ms + 200, 1210);                            /* ambiguous: no sample */
        mt_tcp_get_info(c.t, &in);
        T_EQ(in.srtt_ms, 50);
        T_EQ(in.rto_ms, 200);                                    /* back from the backoff */
    }
}

static void test_fast_retransmit(void) {
    static uint8_t d[6 * MSS];
    tcpv v;
    int i;
    pat(d, sizeof d);
    {
        SETUP(40000, 20000, NULL, NULL);
        c.t->cwnd = 20000;
        T_EQ(mt_tcp_send(c.t, d, sizeof d, 1, 0, 10), (int)sizeof d);
        T_EQ(fl.n, 6);
        fake_clear(&fl);
        ack_to(&c, c.ms + MSS, 20);                              /* segment 1 arrived, 2 lost */
        T_EQ(fl.n, 0);
        for (i = 0; i < 2; i++) ack_to(&c, c.ms + MSS, 21 + (uint32_t)i);
        T_EQ(fl.n, 0);
        ack_to(&c, c.ms + MSS, 23);                              /* third duplicate */
        T_EQ(fl.n, 1);
        T_CHECK(tcp_at(&fl, 0, &v)); T_EQ(v.seq, c.ms + MSS); T_EQ(v.dlen, MSS);
        T_MEMEQ(v.data, d + MSS, MSS);
        T_EQ(c.t->ssthresh, 3650);                               /* flight 7300 / 2 */
        T_EQ(c.t->cwnd, 3650);
        ack_to(&c, c.ms + MSS, 24);                              /* more duplicates: no more */
        T_EQ(fl.n, 1);
        ack_to(&c, c.ms + 6 * MSS, 30);                          /* everything arrived */
        T_EQ(mt_get_stats(c.s)->tcp_rexmt, 1);
        T_EQ(c.t->cwnd, 3650);                                   /* recovery over: cwnd = ssthresh */
        mt_tick(c.s, 5000);
        T_EQ(fl.n, 1);
    }
}

static void test_cwnd_initial_and_growth(void) {
    static uint8_t d[20000];
    tcpv v;
    {
        SETUP(80000, 65535, NULL, NULL);
        T_EQ(mt_tcp_send(c.t, d, sizeof d, 0, 0, 10), 20000);
        T_EQ(fl.n, 3);                                           /* min(4*MSS, max(2*MSS, 4380)) */
        T_CHECK(tcp_at(&fl, 2, &v)); T_EQ(v.dlen, MSS);
        T_EQ(c.t->cwnd, 4380);
        fake_clear(&fl);
        ack_to(&c, c.ms + MSS, 20);                              /* slow start: +1 MSS per ACK */
        T_EQ(c.t->cwnd, 4380 + MSS);
        T_EQ(fl.n, 2);                                           /* flight 2920 -> 5840 */
        fake_clear(&fl);
        ack_to(&c, c.ms + 3 * MSS, 30);
        T_EQ(c.t->cwnd, 4380 + 2 * MSS);                         /* capped at one MSS per ACK */
        T_CHECK(fl.n >= 3);
        c.t->ssthresh = c.t->cwnd;                               /* congestion avoidance from here */
        {
            uint32_t w = c.t->cwnd, acked = 0, base = c.ms + 3 * MSS;
            while (acked < w) {                                  /* one cwnd of ACKs: +1 MSS total */
                acked += MSS;
                ack_to(&c, base + acked, 40);
            }
            T_CHECK(c.t->cwnd >= w + MSS && c.t->cwnd <= w + 2 * MSS);
        }
    }
}

static void test_tcp_persist(void) {
    static uint8_t d[500];
    rec r; tcpv v;
    uint32_t now = 10, want, i;
    memset(&r, 0, sizeof r);
    pat(d, 500);
    {
        SETUP(8192, 0, rec_cb, &r);
        T_EQ(mt_tcp_send(c.t, d, 500, 1, 0, now), 500);
        T_EQ(fl.n, 0);                                           /* closed window: nothing sent */
        want = 1000;                                             /* persist armed, nothing in flight */
        for (i = 0; i < 9; i++) {
            T_EQ(mt_next_deadline(c.s, now), want);
            now = step(&c, now);
            T_EQ(fl.n, (int)i + 1);
            T_CHECK(tcp_at(&fl, (int)i, &v));
            T_EQ(v.seq, c.ms); T_EQ(v.dlen, 1); T_EQ(v.data[0], d[0]);
            ack_to(&c, c.ms, now);                               /* probe not accepted, window 0 */
            want = want * 2 > 60000 ? 60000 : want * 2;
        }
        T_EQ(want, 60000);
        fake_clear(&fl);
        g_pwin = 4096;
        ack_to(&c, c.ms, now + 5);                               /* window opens */
        T_EQ(fl.n, 1);
        T_CHECK(tcp_at(&fl, 0, &v)); T_EQ(v.seq, c.ms); T_EQ(v.dlen, 500);
        T_MEMEQ(v.data, d, 500);
        ack_to(&c, c.ms + 500, now + 10);
        T_EQ(count_ev(&r, MT_TCP_EV_TERMINATE), 0);
        T_EQ(c.t->state, MT_TCP_ESTABLISHED);
        T_CHECK(mt_next_deadline(c.s, now + 10) > 100000);       /* all timers off */
    }
}

static void test_persist_ulp(void) {
    static uint8_t d[500];
    rec r; uint32_t now = 0;
    memset(&r, 0, sizeof r);
    {
        SETUP(8192, 0, rec_cb, &r);                              /* a peer that keeps answering */
        mt_tcp_set_ulp_timeout(c.t, 30, 1);
        mt_tcp_send(c.t, d, 500, 1, 0, now);
        while (now < 200000) {
            now = step(&c, now);
            ack_to(&c, c.ms, now);
        }
        T_EQ(count_ev(&r, MT_TCP_EV_TERMINATE), 0);
        T_EQ(c.t->state, MT_TCP_ESTABLISHED);
    }
    memset(&r, 0, sizeof r);
    {
        SETUP(8192, 0, rec_cb, &r);                              /* a peer that went away */
        r.n = 0;
        mt_tcp_set_ulp_timeout(c.t, 30, 1);
        mt_tcp_send(c.t, d, 500, 1, 0, 0);
        now = 0;
        while (r.n == 0 && now < 100000) now = step(&c, now);
        T_EQ(now, 30000);
        T_EQ(count_ev(&r, MT_TCP_EV_TERMINATE), 1);
    }
}

static void test_ulp_timeout_abort(void) {
    static uint8_t d[100];
    rec r; uint32_t now = 0;
    memset(&r, 0, sizeof r);
    {
        SETUP(8192, 8192, rec_cb, &r);
        r.n = 0;
        mt_tcp_set_ulp_timeout(c.t, 30, 1);
        mt_tcp_send(c.t, d, 100, 1, 0, 0);
        while (r.n == 0 && now < 100000) now = step(&c, now);
        T_EQ(now, 30000);
        T_EQ(r.n, 2);
        T_EQ(r.e[0].ev, MT_TCP_EV_ULP_TIMEOUT);
        T_EQ(r.e[1].ev, MT_TCP_EV_TERMINATE); T_EQ(r.e[1].arg, MT_TERM_ULP_TIMEOUT);
        T_EQ(c.t->state, MT_TCP_CLOSED);
    }
}

static void test_ulp_timeout_report_only(void) {
    static uint8_t d[100];
    rec r; uint32_t now = 0; uint32_t rx;
    memset(&r, 0, sizeof r);
    {
        SETUP(8192, 8192, rec_cb, &r);
        r.n = 0;
        mt_tcp_set_ulp_timeout(c.t, 30, 0);
        mt_tcp_send(c.t, d, 100, 1, 0, 0);
        while (now < 30000) now = step(&c, now);
        T_EQ(now, 30000);
        T_EQ(count_ev(&r, MT_TCP_EV_ULP_TIMEOUT), 1);
        rx = mt_get_stats(c.s)->tcp_rexmt;
        while (now < 60000) now = step(&c, now);
        T_EQ(count_ev(&r, MT_TCP_EV_ULP_TIMEOUT), 2);
        T_CHECK(mt_get_stats(c.s)->tcp_rexmt > rx);              /* still retransmitting */
        T_EQ(count_ev(&r, MT_TCP_EV_TERMINATE), 0);
        T_EQ(c.t->state, MT_TCP_ESTABLISHED);
    }
}

static void test_arp_resolved_kicks_retransmit(void) {
    static uint8_t d[600], rep[60];
    uint32_t seqs[16];
    tcpv v;
    uint16_t n;
    {
        SETUP(8192, 8192, NULL, NULL);
        forget_arp(c.s);
        mt_tcp_send(c.t, d, 600, 1, 0, 10);                      /* too big to be held for ARP */
        T_EQ(data_frames(&fl, seqs, 16), 0);
        fake_clear(&fl);
        n = mk_arp(rep, 2, T_PEER_MAC, T_PEER, T_ME_MAC, T_ME);
        mt_input(c.s, rep, n, 50);
        T_EQ(fl.n, 1);                                           /* immediately, not at the RTO */
        T_CHECK(tcp_at(&fl, 0, &v)); T_EQ(v.seq, c.ms); T_EQ(v.dlen, 600);
        T_EQ(mt_get_stats(c.s)->tcp_rexmt, 1);
    }
    {
        SETUP(8192, 8192, NULL, NULL);
        forget_arp(c.s);
        mt_tcp_send(c.t, d, 100, 1, 0, 10);                      /* small: held, released by the reply */
        fake_clear(&fl);
        n = mk_arp(rep, 2, T_PEER_MAC, T_PEER, T_ME_MAC, T_ME);
        mt_input(c.s, rep, n, 50);
        T_EQ(data_frames(&fl, seqs, 16), 1);                     /* once: not duplicated */
        T_EQ(mt_get_stats(c.s)->tcp_rexmt, 0);
    }
}

static void test_arp_burst_recovers_fast(void) {
    static uint8_t d[6 * MSS], rep[60];
    uint32_t seqs[16];
    uint16_t n;
    int i, k;
    {
        SETUP(40000, 20000, NULL, NULL);
        c.t->cwnd = 6 * MSS;
        forget_arp(c.s);
        mt_tcp_send(c.t, d, sizeof d, 1, 0, 10);                 /* 6 big segments, all lost to ARP */
        fake_clear(&fl);
        n = mk_arp(rep, 2, T_PEER_MAC, T_PEER, T_ME_MAC, T_ME);
        mt_input(c.s, rep, n, 100);
        k = data_frames(&fl, seqs, 16);
        T_EQ(k, 6);                                              /* the whole burst, at once */
        for (i = 0; i < k; i++) T_EQ(seqs[i], c.ms + (uint32_t)i * MSS);
        ack_to(&c, c.ms + 6 * MSS, 150);
        mt_tick(c.s, 100000);
        T_EQ(data_frames(&fl, seqs, 16), 6);
    }
    {
        SETUP(40000, 20000, NULL, NULL);                         /* small first segment is held */
        c.t->cwnd = 6 * MSS;
        forget_arp(c.s);
        mt_tcp_send(c.t, d, 100, 1, 0, 10);
        mt_tcp_send(c.t, d, 5 * MSS, 1, 0, 11);
        fake_clear(&fl);
        n = mk_arp(rep, 2, T_PEER_MAC, T_PEER, T_ME_MAC, T_ME);
        mt_input(c.s, rep, n, 100);
        k = data_frames(&fl, seqs, 16);
        T_EQ(k, 6);                                              /* held one + 5 resent, no duplicate */
        T_EQ(seqs[0], c.ms);
        for (i = 1; i < k; i++) T_EQ(seqs[i], c.ms + 100 + (uint32_t)(i - 1) * MSS);
    }
}

static void test_rto_gobackn_recovers_burst(void) {
    static uint8_t d[6 * MSS];
    tcpv v;
    uint32_t now = 10, end;
    int i, rounds = 0;
    {
        SETUP(40000, 20000, NULL, NULL);
        c.t->cwnd = 6 * MSS;
        mt_tcp_send(c.t, d, sizeof d, 1, 0, now);
        T_EQ(fl.n, 6);
        fake_clear(&fl);                                         /* the whole burst is lost */
        now = 1010;
        mt_tick(c.s, now);                                       /* one RTO */
        T_EQ(fl.n, 1);                                           /* cwnd 1 MSS: first segment only */
        T_EQ(c.t->cwnd, MSS);
        end = c.ms;
        while (end != c.ms + 6 * MSS && rounds < 10) {           /* ack-clocked, one RTT per round */
            for (i = 0; i < fl.n; i++)
                if (tcp_at(&fl, i, &v) && v.seq + v.dlen - c.ms > end - c.ms) end = v.seq + v.dlen;
            fake_clear(&fl);
            now += 20;
            ack_to(&c, end, now);
            rounds++;
        }
        T_EQ(end, c.ms + 6 * MSS);
        T_CHECK(rounds <= 4);
        T_CHECK(now < 1010 + 200);                               /* no second RTO needed */
        T_EQ(mt_get_stats(c.s)->tcp_rexmt, 6);                   /* each segment resent exactly once */
    }
}

static void test_fin_wait2_timeout(void) {
    static uint8_t dd[10];
    rec r;
    memset(&r, 0, sizeof r);
    {
        SETUP(8192, 8192, rec_cb, &r);
        uint8_t f[100];
        uint16_t n;
        uint32_t ms = c.ms;
        r.n = 0;
        mt_tcp_close(c.t, 10);
        ack_to(&c, ms + 1, 20);                                  /* our FIN acked -> FIN_WAIT_2 */
        T_EQ(c.t->state, MT_TCP_FIN_WAIT_2);
        T_EQ(c.t->tmr[MT_TT_TW], 600020);                        /* MT_TCP_FW2_MS after the FIN ack */
        ack_to(&c, ms + 1, 300000);                              /* a pure ACK / probe refreshes */
        n = mk_tcp(f, T_PEER, 23, T_ME, c.lport, c.ps, ms + 1, TF_ACK | TF_PSH, 8192, NULL, 0,
                   dd, 10);
        mt_input(c.s, f, n, 500000);                             /* so does data */
        mt_tick(c.s, 1000000);
        T_EQ(count_ev(&r, MT_TCP_EV_ULP_TIMEOUT), 0);
        T_EQ(count_ev(&r, MT_TCP_EV_TERMINATE), 0);
        mt_tick(c.s, 1100000);                                   /* abort flag set: event, terminate */
        T_EQ(r.e[r.n - 2].ev, MT_TCP_EV_ULP_TIMEOUT);
        T_EQ(r.e[r.n - 1].ev, MT_TCP_EV_TERMINATE); T_EQ(r.e[r.n - 1].arg, MT_TERM_ULP_TIMEOUT);
        T_EQ(c.t->state, MT_TCP_CLOSED);
    }
    memset(&r, 0, sizeof r);
    {
        SETUP(8192, 8192, rec_cb, &r);
        uint32_t ms = c.ms;
        r.n = 0;
        mt_tcp_set_ulp_timeout(c.t, 120, 0);                     /* report only */
        mt_tcp_close(c.t, 10);
        ack_to(&c, ms + 1, 20);
        mt_tick(c.s, 600020);
        T_EQ(count_ev(&r, MT_TCP_EV_ULP_TIMEOUT), 1);
        T_EQ(c.t->state, MT_TCP_FIN_WAIT_2);                     /* keeps waiting */
        mt_tick(c.s, 1200020);
        T_EQ(count_ev(&r, MT_TCP_EV_ULP_TIMEOUT), 2);
        T_EQ(count_ev(&r, MT_TCP_EV_TERMINATE), 0);
    }
}

static void test_deadline_reflects_timers(void) {
    static fake_link fl;
    static uint8_t d[1500];
    mt_stack *s = tstack_new(&fl, T_ME);
    rec r1, r2, r3, r4;
    uint32_t p1, m1, p2, m2, p3, m3, p4, m4;
    mt_tcp *t1, *t2, *t3, *t4;
    mt_tcp_info i1, i2, i3, i4;
    uint8_t f[1600];
    uint16_t n;
    memset(&r1, 0, sizeof r1);
    r2 = r3 = r4 = r1;
    t1 = tconn_established(s, &fl, &r1, &p1, &m1);
    t2 = tconn_established(s, &fl, &r2, &p2, &m2);
    t3 = tconn_established(s, &fl, &r3, &p3, &m3);
    t4 = tconn_established(s, &fl, &r4, &p4, &m4);
    T_CHECK(t1 && t2 && t3 && t4);
    if (!(t1 && t2 && t3 && t4)) return;
    mt_tcp_get_info(t1, &i1); mt_tcp_get_info(t2, &i2);
    mt_tcp_get_info(t3, &i3); mt_tcp_get_info(t4, &i4);
    T_CHECK(mt_next_deadline(s, 0) > 100000);                    /* nothing but ARP aging */
    mt_tcp_send(t1, d, 10, 1, 0, 100);                           /* t1: RTO at 1100 */
    T_EQ(mt_next_deadline(s, 100), 1000);
    mt_tcp_close(t4, 100);                                       /* t4: -> TIME_WAIT at 2120 */
    n = mk_tcp(f, T_PEER, 23, T_ME, i4.lport, p4, m4 + 1, TF_ACK, 8192, NULL, 0, NULL, 0);
    mt_input(s, f, n, 110);
    n = mk_tcp(f, T_PEER, 23, T_ME, i4.lport, p4, m4 + 1, TF_ACK | TF_FIN, 8192, NULL, 0, NULL, 0);
    mt_input(s, f, n, 120);
    T_EQ(t4->state, MT_TCP_TIME_WAIT);
    T_EQ(mt_next_deadline(s, 120), 980);                         /* t1 RTO beats TIME_WAIT */
    n = mk_tcp(f, T_PEER, 23, T_ME, i2.lport, p2, m2, TF_ACK, 8192, NULL, 0, d, 1460);
    mt_input(s, f, n, 200);                                      /* t2: delayed ACK at 300 */
    T_EQ(mt_next_deadline(s, 200), 100);
    mt_tick(s, 300);                                             /* delayed ACK goes out */
    T_EQ(mt_next_deadline(s, 300), 800);
    n = mk_tcp(f, T_PEER, 23, T_ME, i3.lport, p3, m3, TF_ACK, 0, NULL, 0, NULL, 0);
    mt_input(s, f, n, 400);                                      /* t3: zero window, persist at 1400 */
    mt_tcp_send(t3, d, 50, 1, 0, 400);
    T_EQ(mt_next_deadline(s, 400), 700);                         /* still t1's RTO at 1100 */
    n = mk_tcp(f, T_PEER, 23, T_ME, i1.lport, p1, m1 + 10, TF_ACK, 8192, NULL, 0, NULL, 0);
    mt_input(s, f, n, 450);                                      /* t1 acked: RTO off */
    T_EQ(mt_next_deadline(s, 450), 950);                         /* persist (1400) is next */
    mt_tick(s, 1400);                                            /* probe; persist backs off to 3400 */
    T_EQ(mt_next_deadline(s, 1400), 720);                        /* TIME_WAIT at 2120 */
    mt_tick(s, 2120);
    T_EQ(t4->state, MT_TCP_CLOSED);
    T_EQ(mt_next_deadline(s, 2120), 1280);                       /* persist again */
}

T_MAIN(test_rto_retransmit, test_rtt_estimate, test_karn, test_fast_retransmit,
       test_cwnd_initial_and_growth, test_tcp_persist, test_persist_ulp, test_ulp_timeout_abort,
       test_ulp_timeout_report_only, test_arp_resolved_kicks_retransmit,
       test_arp_burst_recovers_fast, test_rto_gobackn_recovers_burst, test_fin_wait2_timeout,
       test_deadline_reflects_timers)
