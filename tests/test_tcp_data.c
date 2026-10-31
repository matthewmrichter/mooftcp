#include "t.h"
#include "net.h"
#include "mt_int.h"
#include "mt_util.h"

/* TCP data transfer: latency rules, ACK policy, Nagle/PSH, segmentation, receive window,
 * urgent data. Every test uses its own connection (established at now=0, peer MSS 1460,
 * peer window 8192; the whole buffer is the receive ring, sends come from caller memory). */

static const uint8_t MSS1460[4] = {2, 4, 0x05, 0xB4};
static uint16_t g_pwin = 8192;

typedef struct {
    rec r;                       /* first: rec_cb treats `user` as a rec* */
    fake_link *fl;
    int got, reply, fl_at_cb;
    uint8_t first;
    uint32_t now;
} cbx;

static void cbx_fn(mt_tcp *t, mt_tcp_event ev, uint32_t arg, void *user) {
    cbx *c = (cbx *)user;
    rec_cb(t, ev, arg, &c->r);
    if (ev == MT_TCP_EV_DATA) {
        const uint8_t *p;
        c->got++;
        c->fl_at_cb = c->fl->n;
        if (mt_tcp_peek(t, 0, &p)) c->first = p[0];
        if (c->reply) T_EQ(tsend(t, (const uint8_t *)"x", 1, 1, 0, c->now), 1);
    }
}

typedef struct { mt_stack *s; fake_link *fl; mt_tcp *t; uint16_t lport; uint32_t ps, ms; } C;

static void inject(C *c, uint32_t seq, uint32_t ack, uint8_t flags, const uint8_t *d,
                   uint16_t n, uint32_t now) {
    static uint8_t buf[1700];
    uint8_t *f = buf + 1;                                   /* odd frame address */
    uint16_t len = mk_tcp(f, T_PEER, 23, T_ME, c->lport, seq, ack, flags, g_pwin, NULL, 0, d, n);
    mt_input(c->s, f, len, now);
}

static void pattern(uint8_t *p, uint32_t from, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) p[i] = (uint8_t)((from + i) * 7u + 3u);
}

/* Segment of n pattern bytes starting at stream offset `from`, sent at seq ps + from. */
static void inject_data(C *c, uint32_t from, uint32_t n, uint8_t extra, uint32_t now) {
    static uint8_t d[1500];
    pattern(d, from, n);
    inject(c, c->ps + from, c->ms, (uint8_t)(TF_ACK | extra), d, (uint16_t)n, now);
}

static int open_conn(C *c, fake_link *fl, uint8_t *buf, uint32_t buflen, uint32_t peer_isn,
                     mt_tcp_cb cb, void *user) {
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
    n = mk_tcp(f, T_PEER, 23, T_ME, v.sport, peer_isn, v.seq + 1, TF_SYN | TF_ACK, g_pwin,
               MSS1460, 4, NULL, 0);
    mt_input(c->s, f, n, 0);
    c->ps = peer_isn + 1;
    c->ms = v.seq + 1;
    fake_clear(fl);
    return 1;
}

static int last_tcp(const fake_link *fl, tcpv *v) {
    return fl->n > 0 && view_tcp(fl->f[fl->n - 1], fl->len[fl->n - 1], v);
}

static int count_ev(const rec *r, int ev) {
    int i, n = 0;
    for (i = 0; i < r->n && i < 32; i++) n += r->e[i].ev == ev;
    return n;
}

#define SETUP(buflen, isn, cb, user) \
    static fake_link fl; static uint8_t buf[32768]; C c; \
    memset(&c, 0, sizeof c); g_pwin = 8192; \
    T_CHECK(open_conn(&c, &fl, buf + 1, (buflen), (isn), (cb), (user)))

static void test_keystroke_immediate(void) {
    rec r; tcpv v; int n0;
    memset(&r, 0, sizeof r);
    {
        SETUP(8192, 5000, rec_cb, &r);
        n0 = fl.n;
        T_EQ(tsend(c.t, (const uint8_t *)"a", 1, 1, 0, 10), 1);
        T_EQ(fl.n, n0 + 1);                             /* gone before any tick */
        T_CHECK(last_tcp(&fl, &v));
        T_EQ(v.flags, TF_ACK | TF_PSH); T_EQ(v.dlen, 1); T_EQ(v.data[0], 'a');
        T_EQ(v.seq, c.ms); T_CHECK(v.checksum_ok);
    }
}

static void test_echo_delivered_in_input(void) {
    cbx x; tcpv v;
    memset(&x, 0, sizeof x);
    {
        SETUP(8192, 5000, cbx_fn, &x);
        x.fl = &fl;
        inject(&c, c.ps, c.ms, TF_ACK | TF_PSH, (const uint8_t *)"e", 1, 10);
        T_EQ(x.got, 1);                                 /* DATA fired inside mt_input */
        T_EQ(x.first, 'e');                             /* and the byte was already readable */
        T_EQ(x.fl_at_cb, 0);
        T_EQ(fl.n, 1);                                  /* exactly one ACK, before return */
        T_CHECK(last_tcp(&fl, &v));
        T_EQ(v.flags, TF_ACK); T_EQ(v.ack, c.ps + 1); T_EQ(v.dlen, 0);
        T_EQ(x.r.e[1].ev, MT_TCP_EV_DATA); T_EQ(x.r.e[1].arg, 1);
    }
}

static void test_ack_piggyback(void) {
    cbx x; tcpv v;
    memset(&x, 0, sizeof x);
    {
        SETUP(8192, 5000, cbx_fn, &x);
        x.fl = &fl; x.reply = 1; x.now = 10;
        inject(&c, c.ps, c.ms, TF_ACK | TF_PSH, (const uint8_t *)"e", 1, 10);
        T_EQ(fl.n, 1);                                  /* one frame: data + ACK together */
        T_CHECK(last_tcp(&fl, &v));
        T_EQ(v.dlen, 1); T_EQ(v.data[0], 'x'); T_EQ(v.ack, c.ps + 1);
        T_EQ(v.flags & TF_ACK, TF_ACK);
    }
}

static void test_full_segment_ack_every_second(void) {
    rec r; tcpv v;
    memset(&r, 0, sizeof r);
    {
        SETUP(8192, 5000, rec_cb, &r);
        c.t->qack = MT_TCP_QUICKACK;                   /* past the quick-ACK phase */
        inject_data(&c, 0, 1460, 0, 1000);
        T_EQ(fl.n, 0);                                  /* first full segment: delayed */
        T_EQ(mt_next_deadline(c.s, 1000), 100);
        inject_data(&c, 1460, 1460, 0, 1010);
        T_EQ(fl.n, 1);                                  /* second: ACK both */
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.ack, c.ps + 2920); T_EQ(v.flags, TF_ACK);
        T_EQ(mt_tcp_deadline(c.s, 1010), MT_NO_DEADLINE);
        inject_data(&c, 2920, 1460, 0, 2000);
        T_EQ(fl.n, 1);
        mt_tick(c.s, 2099);
        T_EQ(fl.n, 1);                                  /* not yet */
        mt_tick(c.s, 2100);
        T_EQ(fl.n, 2);                                  /* delayed ACK at +100 ms */
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.ack, c.ps + 4380);
        T_EQ(count_ev(&r, MT_TCP_EV_DATA), 3);
    }
}

static void test_nagle_with_push_off(void) {
    rec r; tcpv v;
    memset(&r, 0, sizeof r);
    {
        SETUP(8192, 5000, rec_cb, &r);
        T_EQ(tsend(c.t, (const uint8_t *)"x", 1, 1, 0, 10), 1);
        T_EQ(fl.n, 1);
        T_EQ(tsend(c.t, (const uint8_t *)"y", 1, 0, 0, 11), 1);
        T_EQ(fl.n, 1);                                  /* held: data unacked, no push */
        mt_tick(c.s, 50);
        T_EQ(fl.n, 1);
        inject(&c, c.ps, c.ms + 1, TF_ACK, NULL, 0, 60);
        T_EQ(fl.n, 2);
        T_CHECK(last_tcp(&fl, &v));
        T_EQ(v.dlen, 1); T_EQ(v.data[0], 'y'); T_EQ(v.seq, c.ms + 1);
        /* push during a backlog stays immediate */
        T_EQ(tsend(c.t, (const uint8_t *)"z", 1, 1, 0, 61), 1);
        T_EQ(fl.n, 3);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.data[0], 'z');
    }
}

static void test_segmentation(void) {
    rec r; tcpv v;
    static uint8_t d[5000], got[5000];
    int i;
    uint32_t off = 0;
    memset(&r, 0, sizeof r);
    pattern(d, 0, sizeof d);
    {
        SETUP(24000, 5000, rec_cb, &r);
        T_EQ(tsend(c.t, d, 5000, 0, 0, 10), 5000);
        T_EQ(fl.n, 3);                                  /* initial cwnd 4380: three full segments */
        for (i = 0; i < 3; i++) {
            static const uint16_t want[3] = {1460, 1460, 1460};
            T_CHECK(view_tcp(fl.f[i], fl.len[i], &v));
            T_EQ(v.dlen, want[i]); T_EQ(v.seq, c.ms + off); T_CHECK(v.checksum_ok);
            memcpy(got + off, v.data, v.dlen);
            off += v.dlen;
        }
        inject(&c, c.ps, c.ms + 4380, TF_ACK, NULL, 0, 20);   /* all acked: the 620-byte tail goes */
        T_EQ(fl.n, 4);
        T_CHECK(view_tcp(fl.f[3], fl.len[3], &v));
        T_EQ(v.dlen, 620); T_EQ(v.seq, c.ms + off); T_CHECK(v.checksum_ok);
        memcpy(got + off, v.data, v.dlen);
        off += v.dlen;
        T_EQ(off, 5000);
        T_MEMEQ(got, d, 5000);
        T_EQ(v.flags & TF_PSH, TF_PSH);
    }
}

static int g_done_n, g_done_st;
static void done_fn(void *ud, int st) { (void)ud; g_done_n++; g_done_st = st; }

/* A request bigger than the peer window flows as the window allows; done waits for the last ACK. */
static void test_send_window_limited_request(void) {
    rec r; tcpv v;
    static uint8_t d[2000];
    memset(&r, 0, sizeof r);
    pattern(d, 0, sizeof d);
    {
        SETUP(3072, 5000, rec_cb, &r);
        g_pwin = 1024;
        inject(&c, c.ps, c.ms, TF_ACK, NULL, 0, 5);     /* window update: 1024 */
        T_EQ(c.t->snd_wnd, 1024);
        g_done_n = 0;
        T_EQ(tsend_cb(c.t, d, 2000, 0, 0, done_fn, NULL, 10), 2000);
        T_EQ(fl.n, 1);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.dlen, 1024); T_MEMEQ(v.data, d, 1024);
        inject(&c, c.ps, c.ms + 1024, TF_ACK, NULL, 0, 20);
        T_EQ(g_done_n, 0);                              /* half acked: not done */
        T_EQ(fl.n, 2);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.dlen, 976); T_EQ(v.seq, c.ms + 1024);
        T_MEMEQ(v.data, d + 1024, 976); T_EQ(v.flags & TF_PSH, TF_PSH);
        inject(&c, c.ps, c.ms + 1024, TF_ACK, NULL, 0, 21);     /* duplicate: still not done */
        T_EQ(g_done_n, 0);
        inject(&c, c.ps, c.ms + 2000, TF_ACK, NULL, 0, 30);
        T_EQ(g_done_n, 1); T_EQ(g_done_st, MT_OK);
        T_EQ(c.t->sq_n, 0);
    }
}

static void test_recv_peek_consume_window(void) {
    rec r; tcpv v; const uint8_t *p;
    static uint8_t want[3000];
    pattern(want, 0, sizeof want);
    memset(&r, 0, sizeof r);
    {
        SETUP(3072, 5000, rec_cb, &r);                  /* receive ring 3072 */
        c.t->qack = MT_TCP_QUICKACK;                   /* past the quick-ACK phase */
        inject_data(&c, 0, 1460, 0, 10);
        inject_data(&c, 1460, 1460, 0, 11);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.win, 3072 - 2920);
        inject_data(&c, 2920, 80, 0, 12);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.win, 72);    /* dropped by exactly 3000 */
        T_EQ(fl.n, 2);
        T_EQ(mt_tcp_peek(c.t, 0, &p), 3000);
        T_MEMEQ(p, want, 3000);
        T_EQ(mt_tcp_peek(c.t, 2990, &p), 10);
        T_EQ(p[0], want[2990]);
        T_EQ(mt_tcp_peek(c.t, 3000, &p), 0);
        mt_tcp_consume(c.t, 3000, 20);
        T_EQ(fl.n, 3);                                  /* window update: 72 -> 3072 */
        T_CHECK(last_tcp(&fl, &v));
        T_EQ(v.flags, TF_ACK); T_EQ(v.win, 3072); T_EQ(v.ack, c.ps + 3000); T_EQ(v.dlen, 0);
    }
}

static void test_ooo_dropped_dup_ack(void) {
    rec r; tcpv v; mt_tcp_info in;
    memset(&r, 0, sizeof r);
    {
        SETUP(8192, 5000, rec_cb, &r);
        inject(&c, c.ps + 100, c.ms, TF_ACK, (const uint8_t *)"0123456789", 10, 10);
        T_EQ(count_ev(&r, MT_TCP_EV_DATA), 0);
        T_EQ(fl.n, 1);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.flags, TF_ACK); T_EQ(v.ack, c.ps); T_EQ(v.dlen, 0);
        mt_tcp_get_info(c.t, &in); T_EQ(in.rcv_avail, 0);
        inject(&c, c.ps, c.ms, TF_ACK, (const uint8_t *)"abc", 3, 11);    /* in order: accepted */
        mt_tcp_get_info(c.t, &in); T_EQ(in.rcv_avail, 3);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.ack, c.ps + 3);
    }
}

static void test_tcp_zero_window_resume(void) {
    rec r; tcpv v; static uint8_t out[3072], want[3072];
    pattern(want, 0, sizeof want);
    memset(&r, 0, sizeof r);
    {
        SETUP(3072, 5000, rec_cb, &r);                  /* receive ring 3072 */
        inject_data(&c, 0, 1460, 0, 10);
        inject_data(&c, 1460, 1460, 0, 11);
        inject_data(&c, 2920, 152, 0, 12);              /* fills the ring exactly */
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.win, 0); T_EQ(v.ack, c.ps + 3072);
        T_EQ(count_ev(&r, MT_TCP_EV_DATA), 3);
        fake_clear(&fl);
        inject_data(&c, 3072, 100, 0, 13);              /* no room: dropped, zero window ACK */
        T_EQ(count_ev(&r, MT_TCP_EV_DATA), 3);
        T_EQ(fl.n, 1);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.win, 0); T_EQ(v.ack, c.ps + 3072);
        T_EQ(mt_tcp_recv(c.t, out, 3072, 20), 3072);
        T_MEMEQ(out, want, 3072);
        T_EQ(fl.n, 2);                                  /* window update */
        T_CHECK(last_tcp(&fl, &v)); T_CHECK(v.win >= 1460); T_EQ(v.win, 3072);
        inject_data(&c, 3072, 100, 0, 30);              /* now accepted */
        T_EQ(count_ev(&r, MT_TCP_EV_DATA), 4);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.ack, c.ps + 3172);
    }
}

static void test_tcp_seq_wrap(void) {
    rec r; tcpv v; uint32_t i;
    static uint8_t out[10000], want[10000];
    uint32_t got = 0;
    pattern(want, 0, sizeof want);
    memset(&r, 0, sizeof r);
    {
        SETUP(24000, 0xFFFFF000u, rec_cb, &r);          /* receive ring 24000 */
        T_EQ(c.ps, 0xFFFFF001u);
        for (i = 0; i < 10; i++) inject_data(&c, i * 1000, 1000, 0, 10 + i);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.ack, 0xFFFFF001u + 10000u);   /* wrapped past 2^32 */
        T_CHECK(v.ack < 0xFFFFF001u);
        while (got < 10000) {
            uint32_t n = mt_tcp_recv(c.t, out + got, 700, 100);
            T_CHECK(n > 0);
            if (!n) break;
            got += n;
        }
        T_EQ(got, 10000);
        T_MEMEQ(out, want, 10000);
    }
}

static void fix_cksum(uint8_t *f, uint16_t flen) {
    uint8_t *tp = f + 34;
    uint16_t tl = (uint16_t)(flen - 34);
    mt_wr16(tp + 16, 0);
    mt_wr16(tp + 16, mt_cksum_fold(mt_cksum_add(mt_tcp_pseudo(T_PEER, T_ME, tl), tp, tl)));
}

static void test_urgent_receive(void) {
    rec r; mt_tcp_info in; uint32_t mark = 99; uint8_t f[200], out[16];
    uint16_t n;
    memset(&r, 0, sizeof r);
    {
        SETUP(8192, 5000, rec_cb, &r);
        T_EQ(mt_tcp_urgent(c.t, &mark), 0);
        n = mk_tcp(f, T_PEER, 23, T_ME, c.lport, c.ps, c.ms, TF_ACK | TF_URG | TF_PSH, 8192,
                   NULL, 0, (const uint8_t *)"0123456789", 10);
        mt_wr16(f + 34 + 18, 5);
        fix_cksum(f, n);
        mt_input(c.s, f, n, 10);
        T_EQ(count_ev(&r, MT_TCP_EV_URGENT), 1);
        T_EQ(r.e[1].ev, MT_TCP_EV_URGENT); T_EQ(r.e[1].arg, 5);     /* URGENT before DATA */
        T_EQ(r.e[2].ev, MT_TCP_EV_DATA);
        T_EQ(mt_tcp_urgent(c.t, &mark), 1); T_EQ(mark, 5);
        T_EQ(mt_tcp_recv(c.t, out, 3, 11), 3);
        T_EQ(mt_tcp_urgent(c.t, &mark), 1); T_EQ(mark, 2);
        T_EQ(mt_tcp_recv(c.t, out, 2, 12), 2);
        T_EQ(mt_tcp_urgent(c.t, &mark), 0);
        mt_tcp_get_info(c.t, &in); T_EQ(in.rcv_avail, 5);
        mt_input(c.s, f, n, 13);                                     /* retransmission */
        T_EQ(count_ev(&r, MT_TCP_EV_URGENT), 1);                     /* pointer did not advance */
        T_EQ(mt_tcp_urgent(c.t, &mark), 0);
    }
}

static void test_urgent_send(void) {
    rec r; tcpv v;
    memset(&r, 0, sizeof r);
    {
        SETUP(8192, 5000, rec_cb, &r);
        T_EQ(tsend(c.t, (const uint8_t *)"abc", 3, 1, 1, 10), 3);
        T_EQ(fl.n, 1);
        T_CHECK(last_tcp(&fl, &v));
        T_EQ(v.flags & TF_URG, TF_URG); T_EQ(v.dlen, 3);
        T_EQ(mt_rd16(v.data - 2), 3);                   /* pointer = seq + len, relative */
        T_CHECK(v.checksum_ok);
        inject(&c, c.ps, c.ms + 3, TF_ACK, NULL, 0, 20);
        T_EQ(tsend(c.t, (const uint8_t *)"d", 1, 1, 0, 21), 1);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.flags & TF_URG, 0); T_EQ(mt_rd16(v.data - 2), 0);
    }
}

static void test_data_after_fin_rejected(void) {
    rec r; mt_tcp_info in;
    memset(&r, 0, sizeof r);
    {
        SETUP(8192, 5000, rec_cb, &r);
        inject(&c, c.ps, c.ms, TF_ACK | TF_FIN, NULL, 0, 10);
        T_EQ(count_ev(&r, MT_TCP_EV_CLOSING), 1);
        mt_tcp_get_info(c.t, &in); T_EQ(in.state, MT_TCP_CLOSE_WAIT);
        inject(&c, c.ps + 1, c.ms, TF_ACK, (const uint8_t *)"late", 4, 11);
        T_EQ(count_ev(&r, MT_TCP_EV_DATA), 0);
        mt_tcp_get_info(c.t, &in); T_EQ(in.rcv_avail, 0);
        inject(&c, c.ps, c.ms, TF_ACK, (const uint8_t *)"old", 3, 12);    /* below the FIN too */
        T_EQ(count_ev(&r, MT_TCP_EV_DATA), 0);
        mt_tcp_get_info(c.t, &in); T_EQ(in.rcv_avail, 0);
    }
}

static void test_bad_checksum_changes_nothing(void) {
    rec r; mt_tcp_info in; uint8_t f[1700]; uint16_t n; static uint8_t d[1000];
    uint32_t drops;
    memset(&r, 0, sizeof r);
    pattern(d, 0, sizeof d);
    {
        SETUP(8192, 5000, rec_cb, &r);
        drops = mt_get_stats(c.s)->tcp_in_drops;
        n = mk_tcp(f, T_PEER, 23, T_ME, c.lport, c.ps, c.ms, TF_ACK | TF_PSH, 8192, NULL, 0, d,
                   1000);
        f[n - 1] ^= 0x55;                               /* corrupt the last data byte */
        mt_input(c.s, f, n, 10);
        T_EQ(mt_get_stats(c.s)->tcp_in_drops, drops + 1);
        T_EQ(fl.n, 0); T_EQ(r.n, 1);
        mt_tcp_get_info(c.t, &in); T_EQ(in.rcv_avail, 0); T_EQ(in.rcv_wnd, 8192);
        f[n - 1] ^= 0x55;                               /* repaired: accepted at the same seq */
        mt_input(c.s, f, n, 11);
        mt_tcp_get_info(c.t, &in); T_EQ(in.rcv_avail, 1000);
        T_EQ(fl.n, 1);
    }
}

static void test_rx_ring_wrap_and_overlap(void) {
    rec r; mt_tcp_info in; static uint8_t out[3072], want[1200]; tcpv v;
    uint32_t i;
    memset(&r, 0, sizeof r);
    pattern(want, 3071, sizeof want);
    {
        SETUP(3072, 5000, rec_cb, &r);                  /* receive ring 3072 */
        inject_data(&c, 0, 1460, 0, 10);
        inject_data(&c, 1460, 1460, 0, 11);
        inject_data(&c, 2920, 151, 0, 12);
        T_EQ(mt_tcp_recv(c.t, out, 3072, 13), 3071);    /* ring head now at the odd index 3071 */
        fake_clear(&fl);
        inject_data(&c, 3071, 1000, 0, 20);             /* first chunk 1 byte, second 999 */
        mt_tcp_get_info(c.t, &in); T_EQ(in.rcv_avail, 1000);
        T_EQ(fl.n, 1);
        /* retransmission overlapping 7 old bytes (odd skip) plus 100 new ones */
        inject_data(&c, 3071 + 993, 107, 0, 21);
        mt_tcp_get_info(c.t, &in); T_EQ(in.rcv_avail, 1100);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.ack, c.ps + 3071 + 1100);
        T_EQ(mt_tcp_recv(c.t, out, 1100, 22), 1100);
        for (i = 0; i < 1100; i++) T_CHECK(out[i] == want[i]);
    }
}

T_MAIN(test_keystroke_immediate, test_echo_delivered_in_input, test_ack_piggyback,
       test_full_segment_ack_every_second, test_nagle_with_push_off, test_segmentation,
       test_send_window_limited_request, test_recv_peek_consume_window, test_ooo_dropped_dup_ack,
       test_tcp_zero_window_resume, test_tcp_seq_wrap, test_urgent_receive, test_urgent_send,
       test_data_after_fin_rejected, test_bad_checksum_changes_nothing,
       test_rx_ring_wrap_and_overlap)
