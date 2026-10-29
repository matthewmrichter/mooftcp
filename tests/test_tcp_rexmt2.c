#include "t.h"
#include "net.h"
#include "mt_int.h"
#include "mt_util.h"

/* Stale (reordered) ACKs must not move snd_una backwards, free unacked data or grow cwnd. */

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
        T_EQ(mt_tcp_send(c.t, d, sizeof d, 1, 0, 10), (int)sizeof d);
        T_EQ(fl.n, 3);
        inject(&c, c.ps, c.ms + 2 * MSS, 8192, 20);
        una = c.t->snd_una; used = c.t->sndq.used; cwnd = c.t->cwnd;
        ssthresh = c.t->ssthresh; nxt = c.t->snd_nxt;
        rx = mt_get_stats(c.s)->tcp_rexmt;
        T_EQ(una, c.ms + 2 * MSS); T_EQ(used, MSS);
        fake_clear(&fl);
        inject(&c, c.ps, c.ms + MSS, 8192, 21);                  /* reordered older ACK */
        inject(&c, c.ps, c.ms + MSS, 8192, 22);
        inject(&c, c.ps, c.ms + MSS, 8192, 23);                  /* and again: not dup ACKs either */
        T_EQ(c.t->snd_una, una);
        T_EQ(c.t->sndq.used, used);
        T_EQ(c.t->cwnd, cwnd);
        T_EQ(c.t->ssthresh, ssthresh);
        T_EQ(c.t->snd_nxt, nxt);
        T_EQ(c.t->dupacks, 0);
        T_EQ(mt_get_stats(c.s)->tcp_rexmt, rx);
        T_EQ(fl.n, 0);
        {   /* the unacked bytes are still the right ones */
            const uint8_t *p;
            T_EQ(mt_ring_peek(&c.t->sndq, 0, &p), MSS);
            T_MEMEQ(p, d + 2 * MSS, MSS);
        }
        inject(&c, c.ps, c.ms + 3 * MSS, 8192, 30);              /* the real ACK still works */
        T_EQ(c.t->sndq.used, 0);
        T_EQ(c.t->snd_una, c.ms + 3 * MSS);
    }
}

static void test_stale_ack_window_rule(void) {
    static uint8_t d[3 * MSS];
    {
        SETUP();
        mt_tcp_send(c.t, d, sizeof d, 1, 0, 10);
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

T_MAIN(test_stale_ack_ignored, test_stale_ack_window_rule)
