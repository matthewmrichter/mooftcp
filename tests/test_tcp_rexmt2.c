#include "t.h"
#include "net.h"
#include "mt_int.h"
#include "mt_util.h"

/* Stale (reordered) ACKs must not move snd_una backwards, free unacked data or grow cwnd.
 * Go-back-N recovery re-reads the caller's segments across odd segment boundaries. */

static const uint8_t MSS1460[4] = {2, 4, 0x05, 0xB4};
#define MSS 1460u

typedef struct { mt_stack *s; fake_link *fl; mt_tcp *t; uint16_t lport; uint32_t ps, ms; } C;

static void inject(C *c, uint32_t seq, uint32_t ack, uint16_t win, uint32_t now) {
    static uint8_t buf[200];
    uint8_t *f = buf + 1;
    uint16_t len = mk_tcp(f, T_PEER, 23, T_ME, c->lport, seq, ack, TF_ACK, win, NULL, 0, NULL, 0);
    mt_input(c->s, f, len, now);
}

static int open_conn(C *c, fake_link *fl, uint8_t *buf, uint32_t buflen) {
    tcpv v;
    uint8_t f[200];
    uint16_t n;
    c->fl = fl;
    c->s = tstack_new(fl, T_ME);
    mt_arp_insert(c->s, T_PEER, T_PEER_MAC, 0);
    c->t = mt_tcp_create(c->s, buf, buflen, NULL, NULL);
    if (!c->t || mt_tcp_open(c->t, T_PEER, 23, 0, 0) != MT_OK) return 0;
    if (fl->n != 1 || !view_tcp(fl->f[0], fl->len[0], &v)) return 0;
    c->lport = v.sport;
    n = mk_tcp(f, T_PEER, 23, T_ME, v.sport, 5000, v.seq + 1, TF_SYN | TF_ACK, 8192, MSS1460, 4,
               NULL, 0);
    mt_input(c->s, f, n, 0);
    c->ps = 5001;
    c->ms = v.seq + 1;
    fake_clear(fl);
    return 1;
}

#define SETUP() \
    static fake_link fl; static uint8_t buf[40000]; C c; \
    memset(&c, 0, sizeof c); \
    T_CHECK(open_conn(&c, &fl, buf + 1, 32000))

static void test_stale_ack_ignored(void) {
    static uint8_t d[3 * MSS];
    uint32_t una, used, cwnd, ssthresh, nxt, rx;
    uint32_t i;
    for (i = 0; i < sizeof d; i++) d[i] = (uint8_t)(i * 3u);
    {
        SETUP();
        T_EQ(tsend(c.t, d, sizeof d, 1, 0, 10), (int)sizeof d);
        T_EQ(fl.n, 3);
        inject(&c, c.ps, c.ms + 2 * MSS, 8192, 20);
        una = c.t->snd_una; used = mt_tcp_sq_total(c.t); cwnd = c.t->cwnd;
        ssthresh = c.t->ssthresh; nxt = c.t->snd_nxt;
        rx = mt_get_stats(c.s)->tcp_rexmt;
        T_EQ(una, c.ms + 2 * MSS); T_EQ(used, MSS);
        fake_clear(&fl);
        inject(&c, c.ps, c.ms + MSS, 8192, 21);                  /* reordered older ACK */
        inject(&c, c.ps, c.ms + MSS, 8192, 22);
        inject(&c, c.ps, c.ms + MSS, 8192, 23);                  /* and again: not dup ACKs either */
        T_EQ(c.t->snd_una, una);
        T_EQ(mt_tcp_sq_total(c.t), used);
        T_EQ(c.t->cwnd, cwnd);
        T_EQ(c.t->ssthresh, ssthresh);
        T_EQ(c.t->snd_nxt, nxt);
        T_EQ(c.t->dupacks, 0);
        T_EQ(mt_get_stats(c.s)->tcp_rexmt, rx);
        T_EQ(fl.n, 0);
        /* the cursor still points at the first unacked byte of the caller's buffer */
        T_EQ(c.t->sq_n, 1);
        T_CHECK(c.t->sq[0].segs[c.t->sq_idx].p + c.t->sq_off == d + 2 * MSS);
        inject(&c, c.ps, c.ms + 3 * MSS, 8192, 30);              /* the real ACK still works */
        T_EQ(mt_tcp_sq_total(c.t), 0);
        T_EQ(c.t->sq_n, 0);
        T_EQ(c.t->snd_una, c.ms + 3 * MSS);
    }
}

static void test_stale_ack_window_rule(void) {
    static uint8_t d[3 * MSS];
    {
        SETUP();
        tsend(c.t, d, sizeof d, 1, 0, 10);
        inject(&c, c.ps, c.ms + 2 * MSS, 8192, 20);
        T_EQ(c.t->snd_wnd, 8192);
        T_EQ(c.t->snd_wl2, c.ms + 2 * MSS);
        /* stale ACK (ack < snd_una) with a newer window and a newer peer seq: RFC 793 requires
         * SND.UNA =< SEG.ACK for a window update, so nothing may change */
        inject(&c, c.ps + 5, c.ms + MSS, 100, 21);
        T_EQ(c.t->snd_wnd, 8192);
        T_EQ(c.t->snd_wl1, c.ps);
        T_EQ(c.t->snd_wl2, c.ms + 2 * MSS);
        /* an acceptable ACK with newer seq updates window and markers */
        inject(&c, c.ps + 5, c.ms + 2 * MSS, 4000, 22);
        T_EQ(c.t->snd_wnd, 4000);
        T_EQ(c.t->snd_wl1, c.ps + 5);
        /* an older segment (seq < wl1) with a bigger window must not regress it */
        inject(&c, c.ps, c.ms + 2 * MSS, 9000, 23);
        T_EQ(c.t->snd_wnd, 4000);
        T_EQ(c.t->snd_wl1, c.ps + 5);
    }
}

static int g_done, g_done_st;
static void done_fn(void *ud, int st) { (void)ud; g_done++; g_done_st = st; }

static int frame_is(const fake_link *fl, int i, uint32_t seq, const uint8_t *want, uint16_t n) {
    tcpv v;
    if (!view_tcp(fl->f[i], fl->len[i], &v) || !v.checksum_ok) return 0;
    return v.seq == seq && v.dlen == n && memcmp(v.data, want, n) == 0;
}

/* Segments of 3, 0, 1500 and 7 bytes at odd addresses (request 1: 1510 bytes), then request 2
 * of 2999 bytes: the first segment spans three caller segments, the second spans the request
 * boundary. The whole first flight is lost; the RTO goes back to snd_una and the ACK clock
 * refills the rest; every resent byte equals the caller's, requests complete in order. */
static void test_go_back_n_across_segment_boundaries(void) {
    static uint8_t m1[8], m2[1504], m3[12], m4[3002], want[4509];
    static mt_seg s1[4], s2[1];
    uint32_t i;
    for (i = 0; i < sizeof m2; i++) m2[i] = (uint8_t)(i * 11u + 5u);
    for (i = 0; i < sizeof m4; i++) m4[i] = (uint8_t)(i * 3u + 9u);
    memcpy(m1 + 1, "abc", 3);
    memcpy(m3 + 3, "TAILEND", 7);
    s1[0].p = m1 + 1; s1[0].len = 3;
    s1[1].p = m2;     s1[1].len = 0;
    s1[2].p = m2 + 1; s1[2].len = 1500;
    s1[3].p = m3 + 3; s1[3].len = 7;
    s2[0].p = m4 + 1; s2[0].len = 2999;
    memcpy(want, m1 + 1, 3);
    memcpy(want + 3, m2 + 1, 1500);
    memcpy(want + 1503, m3 + 3, 7);
    memcpy(want + 1510, m4 + 1, 2999);
    g_done = 0;
    {
        SETUP();
        T_EQ(mt_tcp_send_segs(c.t, s1, 4, 0, 0, done_fn, NULL, 10), MT_OK);
        T_EQ(mt_tcp_send_segs(c.t, s2, 1, 1, 0, done_fn, NULL, 10), MT_OK);
        T_EQ(fl.n, 3);       /* request 1 from idle goes whole (1460 + 50), request 2 adds one
                              * full segment; its 1410-byte tail waits (Nagle, not yet pushed) */
        T_CHECK(frame_is(&fl, 0, c.ms, want, MSS));
        T_CHECK(frame_is(&fl, 1, c.ms + MSS, want + MSS, 50));
        T_CHECK(frame_is(&fl, 2, c.ms + 1510, want + 1510, MSS));
        fake_clear(&fl);                                         /* all three lost */
        mt_tick(c.s, 1010);                                      /* RTO: cwnd 1 MSS, from snd_una */
        T_EQ(fl.n, 1);
        T_CHECK(frame_is(&fl, 0, c.ms, want, MSS));
        fake_clear(&fl);
        inject(&c, c.ps, c.ms + MSS, 8192, 1011);                /* slow start: two more resends,
                                                                  * re-cut into full segments */
        T_EQ(fl.n, 2);
        T_CHECK(frame_is(&fl, 0, c.ms + MSS, want + MSS, MSS));  /* crosses the request boundary */
        T_CHECK(frame_is(&fl, 1, c.ms + 2 * MSS, want + 2 * MSS, MSS));
        T_EQ(g_done, 0);
        fake_clear(&fl);
        inject(&c, c.ps, c.ms + 2 * MSS, 8192, 1012);            /* request 1 (1510) acked */
        T_EQ(g_done, 1); T_EQ(g_done_st, MT_OK);
        T_EQ(fl.n, 1);                                           /* new data: the pushed tail */
        T_CHECK(frame_is(&fl, 0, c.ms + 3 * MSS, want + 3 * MSS, 129));
        inject(&c, c.ps, c.ms + 4509, 8192, 1013);
        T_EQ(g_done, 2); T_EQ(g_done_st, MT_OK);
        T_EQ(c.t->sq_n, 0);
        T_EQ(mt_get_stats(c.s)->tcp_rexmt, 3);
    }
}

T_MAIN(test_stale_ack_ignored, test_stale_ack_window_rule, test_go_back_n_across_segment_boundaries)
