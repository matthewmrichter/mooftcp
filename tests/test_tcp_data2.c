#include "t.h"
#include "net.h"
#include "mt_int.h"
#include "mt_util.h"

/* TCP data transfer, part 2: receiver SWS, delayed-ACK cancellation, PSH on full segments,
 * send state errors, events after TERMINATE, data+FIN, and the send queue (mt_tcp_send_segs:
 * caller memory, done on ACK, FIFO of MT_SENDQ_MAX requests). Every test uses its own connection
 * (established at now=0, peer MSS 1460, peer window 8192; the whole buffer is the receive ring). */

static const uint8_t MSS1460[4] = {2, 4, 0x05, 0xB4};
static uint16_t g_pwin = 8192;

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


static void test_sws_no_small_window_updates(void) {
    rec r; tcpv v; mt_tcp_info in;
    memset(&r, 0, sizeof r);
    {
        SETUP(3072, 5000, rec_cb, &r);                  /* receive ring 3072, threshold 1460 */
        inject_data(&c, 0, 1460, 0, 10);
        inject_data(&c, 1460, 1460, 0, 11);
        inject_data(&c, 2920, 152, 0, 12);              /* ring full, window 0 */
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.win, 0);
        fake_clear(&fl);
        mt_tcp_consume(c.t, 100, 20);                   /* 100 bytes free: below min(MSS, ring/2) */
        mt_tcp_consume(c.t, 1000, 21);
        T_EQ(fl.n, 0);                                  /* no silly-window update */
        mt_tcp_consume(c.t, 360, 22);                   /* 1460 free: reaches the threshold */
        T_EQ(fl.n, 1);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.win, 1460);
        mt_tcp_get_info(c.t, &in); T_EQ(in.rcv_wnd, 1460);
        mt_tcp_consume(c.t, 500, 23);                   /* 500 more: held back again */
        T_EQ(fl.n, 1);
        inject_data(&c, 3072, 10, 0, 30);               
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.win, 1450);                   /* edge kept: 1460 - 10, not the 1950 free */
    }
}

static void test_delayed_ack_carried_by_send(void) {
    rec r; tcpv v;
    memset(&r, 0, sizeof r);
    {
        SETUP(8192, 5000, rec_cb, &r);
        inject_data(&c, 0, 1460, 0, 1000);              /* full segment: ACK delayed */
        T_EQ(fl.n, 0);
        T_EQ(tsend(c.t, (const uint8_t *)"q", 1, 1, 0, 1020), 1);
        T_EQ(fl.n, 1);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.ack, c.ps + 1460); T_EQ(v.dlen, 1);
        mt_tick(c.s, 1100);
        T_EQ(fl.n, 1);                                  /* the delayed ACK was cancelled */
        inject_data(&c, 1460, 1460, 0, 2000);           /* next full segment starts a new pair */
        T_EQ(fl.n, 1);
    }
}

static void test_psh_full_segment_acked_now(void) {
    rec r; tcpv v;
    memset(&r, 0, sizeof r);
    {
        SETUP(8192, 5000, rec_cb, &r);
        inject_data(&c, 0, 1460, TF_PSH, 10);
        T_EQ(fl.n, 1);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.ack, c.ps + 1460);
    }
}

static int g_dn, g_dst;
static void done_cnt(void *ud, int st) { (void)ud; g_dn++; g_dst = st; }

static void test_send_state_and_no_events_after_terminate(void) {
    rec r; static uint8_t d[2000];
    memset(&r, 0, sizeof r);
    {
        SETUP(4096, 5000, rec_cb, &r);
        g_dn = 0;
        T_EQ(tsend_cb(c.t, d, 2000, 0, 0, done_cnt, NULL, 10), 2000);
        inject(&c, c.ps, c.ms + 1024, TF_RST | TF_ACK, NULL, 0, 20);
        T_EQ(count_ev(&r, MT_TCP_EV_TERMINATE), 1);
        T_EQ(g_dn, 1); T_EQ(g_dst, MT_ERR_ABORTED);    /* the pending send ends with the RST */
        T_EQ(r.e[r.n - 1].ev, MT_TCP_EV_TERMINATE);     /* ... before TERMINATE (last event) */
        T_EQ(c.t->sq_n, 0);
        T_EQ(tsend(c.t, d, 10, 1, 0, 21), MT_ERR_STATE);
        T_EQ(c.t->flags2, 0);
        T_EQ(mt_tcp_close(c.t, 22), MT_ERR_STATE);
    }
}

static void test_data_with_fin_single_segment(void) {
    rec r; tcpv v; mt_tcp_info in; static uint8_t out[16];
    memset(&r, 0, sizeof r);
    {
        SETUP(8192, 5000, rec_cb, &r);
        inject(&c, c.ps, c.ms, TF_ACK | TF_FIN, (const uint8_t *)"bye", 3, 10);
        T_EQ(r.e[1].ev, MT_TCP_EV_DATA); T_EQ(r.e[2].ev, MT_TCP_EV_CLOSING);
        T_EQ(fl.n, 1);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.ack, c.ps + 4);
        mt_tcp_get_info(c.t, &in); T_EQ(in.state, MT_TCP_CLOSE_WAIT);
        T_EQ(mt_tcp_recv(c.t, out, 16, 11), 3);
        T_MEMEQ(out, "bye", 3);
        T_EQ(tsend(c.t, (const uint8_t *)"ok", 2, 1, 0, 12), 2);   /* half close: still sends */
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.dlen, 2);
    }
}

static void test_nagle_holds_tail_in_flight(void) {
    rec r; tcpv v; static uint8_t d[1560];
    memset(&r, 0, sizeof r);
    pattern(d, 0, sizeof d);
    {
        SETUP(8192, 5000, rec_cb, &r);
        T_EQ(tsend(c.t, (const uint8_t *)"x", 1, 1, 0, 10), 1);
        T_EQ(fl.n, 1);                                  /* 1 byte now in flight */
        T_EQ(tsend(c.t, d, 1560, 0, 0, 11), 1560);
        T_EQ(fl.n, 2);                                  /* one full segment only */
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.dlen, 1460);
        inject(&c, c.ps, c.ms + 1, TF_ACK, NULL, 0, 12);   /* ACKs x only: 1460 still in flight */
        T_EQ(fl.n, 2);                                  /* 100-byte tail still held */
        inject(&c, c.ps, c.ms + 1461, TF_ACK, NULL, 0, 13);
        T_EQ(fl.n, 3);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.dlen, 100); T_EQ(v.seq, c.ms + 1461);
        /* with push the tail goes at once, data still in flight */
        T_EQ(tsend(c.t, d, 1560, 1, 0, 20), 1560);
        T_EQ(fl.n, 5);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.dlen, 100);
    }
}

/* RFC 5681 4.2: while a hole is open (data seen beyond rcv_nxt), every segment that advances
 * rcv_nxt is ACKed at once, until rcv_nxt reaches the end of the highest out-of-order segment.
 * The receiver keeps no out-of-order data, so the sender resends each hole segment alone. */
static void gap_scenario(uint32_t isn) {
    rec r; tcpv v; mt_tcp_info in;
    uint32_t i, ooo_end;
    memset(&r, 0, sizeof r);
    {
        SETUP(6144, isn, rec_cb, &r);                   /* receive ring 6144 = 4 segments + 304 */
        ooo_end = c.ps + 4u * 1460u;
        inject_data(&c, 3u * 1460u, 1460, 0, 10);       /* segment 3 first: sets the high mark */
        inject_data(&c, 1460, 1460, 0, 11);             /* segment 1: lower, must not lower it */
        T_EQ(fl.n, 2);                                  /* two duplicate ACKs */
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.ack, c.ps);
        T_CHECK(c.t->flags2 & MT_T2_GAP);
        T_EQ(c.t->ooo_hi, ooo_end);
        fake_clear(&fl);
        for (i = 0; i < 3; i++) {                       /* the hole segments 0, 1, 2 arrive alone */
            inject_data(&c, i * 1460u, 1460, 0, 20 + i);
            T_EQ(fl.n, (int)i + 1);                     /* ACKed before mt_input returned */
            T_CHECK(last_tcp(&fl, &v)); T_EQ(v.ack, c.ps + (i + 1u) * 1460u);
            T_CHECK(!(c.t->tmask & (1u << MT_TT_DELACK)));
            T_CHECK(!(c.t->flags2 & MT_T2_ACKHALF));
            T_CHECK(c.t->flags2 & MT_T2_GAP);           /* below the high mark: persists */
        }
        inject_data(&c, 3u * 1460u, 1460, 0, 30);       /* reaches ooo_hi: immediate, then done */
        T_EQ(fl.n, 4);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.ack, ooo_end);
        T_CHECK(!(c.t->flags2 & MT_T2_GAP));
        mt_tcp_get_info(c.t, &in); T_EQ(in.rcv_avail, 4u * 1460u);
        mt_tcp_consume(c.t, 4u * 1460u, 31);
        fake_clear(&fl);
        inject_data(&c, 4u * 1460u, 1460, 0, 40);       /* normal policy again: delayed ACK */
        T_EQ(fl.n, 0);
        T_CHECK(c.t->tmask & (1u << MT_TT_DELACK));
        inject_data(&c, 5u * 1460u, 1460, 0, 41);       /* every second segment */
        T_EQ(fl.n, 1);
    }
}

static void test_gap_acks_every_fill_segment(void) { gap_scenario(5000); }

/* The high mark crosses 2^32: the compares must be wrap-safe. */
static void test_gap_high_mark_wraps(void) { gap_scenario(0xFFFFFA00u); }


/* ---- send queue (mt_tcp_send_segs) ---- */

/* Ordered log shared by done callbacks (100 + request id, status in st[]) and TCP events. */
typedef struct { rec r; int ord[32], st[32], n; mt_tcp *t; int queue_on_done; } qlog;
static mt_seg g_extra_seg[1];
static void qlog_ev(mt_tcp *t, mt_tcp_event ev, uint32_t arg, void *user) {
    qlog *q = (qlog *)user;
    rec_cb(t, ev, arg, &q->r);
    if (q->n < 32) { q->ord[q->n] = (int)ev; q->st[q->n] = (int)arg; }
    q->n++;
}
static qlog *g_q;
static void qlog_done(void *ud, int st) {
    qlog *q = g_q;
    int id = (int)(intptr_t)ud;
    if (q->n < 32) { q->ord[q->n] = 100 + id; q->st[q->n] = st; }
    q->n++;
    if (q->queue_on_done && id == 1) {               /* a slot is free: queue request 3 now */
        static const uint8_t z[3] = {'Z', 'Z', 'Z'};
        g_extra_seg[0].p = z; g_extra_seg[0].len = 3;
        T_EQ(mt_tcp_send_segs(q->t, g_extra_seg, 1, 1, 0, qlog_done, (void *)(intptr_t)3, 25),
             MT_OK);
    }
}

/* Concatenated payload of the data frames in fl (in order). */
static uint32_t wire_bytes(const fake_link *fl, uint8_t *out, uint32_t max) {
    int i; uint32_t n = 0; tcpv v;
    for (i = 0; i < fl->n; i++)
        if (view_tcp(fl->f[i], fl->len[i], &v) && v.dlen && n + v.dlen <= max) {
            T_CHECK(v.checksum_ok);
            memcpy(out + n, v.data, v.dlen);
            n += v.dlen;
        }
    return n;
}

/* Wire bytes equal the segments; canaries around each segment, the segment bytes and the mt_seg
 * array itself are never written by the stack (also across retransmission and the final ACK). */
static void test_send_segs_no_copy(void) {
    rec r;
    static uint8_t mem[64], snap[64], got[64];
    static mt_seg sg[4], sgsnap[4];
    static const uint8_t want[] = "abcdefgXYyz";
    uint32_t i;
    memset(&r, 0, sizeof r);
    for (i = 0; i < sizeof mem; i++) mem[i] = 0xA5;
    memcpy(mem + 3, "abcdefg", 7);               /* odd address, odd length */
    memcpy(mem + 13, "X", 1);                    /* odd address, 1 byte */
    memcpy(mem + 18, "Yyz", 3);                  /* even address, odd length */
    sg[0].p = mem + 3;  sg[0].len = 7;
    sg[1].p = mem + 13; sg[1].len = 1;
    sg[2].p = NULL;     sg[2].len = 0;           /* empty entry: skipped, pointer never used */
    sg[3].p = mem + 18; sg[3].len = 3;
    memcpy(snap, mem, sizeof mem);
    memcpy(sgsnap, sg, sizeof sg);
    {
        SETUP(4096, 5000, rec_cb, &r);
        T_EQ(mt_tcp_send_segs(c.t, sg, 4, 1, 0, NULL, NULL, 10), MT_OK);
        T_EQ(fl.n, 1);
        T_EQ(wire_bytes(&fl, got, sizeof got), 11);
        T_MEMEQ(got, want, 11);
        mt_tick(c.s, 10 + 1000);                 /* RTO: retransmitted from the same memory */
        T_EQ(fl.n, 2);
        T_EQ(wire_bytes(&fl, got, sizeof got), 22);
        T_MEMEQ(got + 11, want, 11);
        inject(&c, c.ps, c.ms + 11, TF_ACK, NULL, 0, 1020);
        T_EQ(c.t->sq_n, 0);
        T_MEMEQ(mem, snap, sizeof mem);
        T_MEMEQ(sg, sgsnap, sizeof sg);
    }
}

/* The first transmission is lost; the RTO resend re-reads the caller's (unchanged) memory, and
 * go-back-N from snd_una refills across the segment boundary. */
static void test_send_segs_retransmit_from_caller(void) {
    rec r; tcpv v;
    static uint8_t a[1001], b[1003], first[2000], again[2000];
    static mt_seg sg[2];
    uint32_t i;
    memset(&r, 0, sizeof r);
    for (i = 0; i < sizeof a; i++) a[i] = (uint8_t)(i * 13u + 1u);
    for (i = 0; i < sizeof b; i++) b[i] = (uint8_t)(i * 7u + 2u);
    sg[0].p = a + 1; sg[0].len = 1000;            /* odd address */
    sg[1].p = b + 3; sg[1].len = 999;             /* odd address, odd length: 1999 bytes */
    {
        SETUP(4096, 5000, rec_cb, &r);
        T_EQ(mt_tcp_send_segs(c.t, sg, 2, 1, 0, NULL, NULL, 10), MT_OK);
        T_EQ(fl.n, 2);                            /* 1460 + 539 */
        T_EQ(wire_bytes(&fl, first, sizeof first), 1999);
        T_MEMEQ(first, a + 1, 1000);
        T_MEMEQ(first + 1000, b + 3, 999);
        fake_clear(&fl);                          /* both lost */
        mt_tick(c.s, 1010);                       /* RTO: cwnd 1 MSS, first segment again */
        T_EQ(fl.n, 1);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.seq, c.ms); T_EQ(v.dlen, 1460);
        inject(&c, c.ps, c.ms + 1460, TF_ACK, NULL, 0, 1020);
        T_EQ(fl.n, 2);                            /* then the rest */
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.seq, c.ms + 1460); T_EQ(v.dlen, 539);
        T_EQ(wire_bytes(&fl, again, sizeof again), 1999);
        T_MEMEQ(again, first, 1999);
        T_EQ(mt_get_stats(c.s)->tcp_rexmt, 2);
    }
}

static void test_send_done_fires_on_ack_only(void) {
    rec r;
    static const uint8_t d[3000] = {1};
    memset(&r, 0, sizeof r);
    {
        SETUP(4096, 5000, rec_cb, &r);
        g_dn = 0; g_dst = 99;
        T_EQ(tsend_cb(c.t, d, 3000, 1, 0, done_cnt, NULL, 10), 3000);
        T_EQ(fl.n, 3);                            /* all transmitted ... */
        T_EQ(g_dn, 0);                            /* ... but nothing acked */
        mt_tick(c.s, 500);
        T_EQ(g_dn, 0);
        inject(&c, c.ps, c.ms + 1460, TF_ACK, NULL, 0, 600);
        inject(&c, c.ps, c.ms + 2999, TF_ACK, NULL, 0, 601);   /* one byte short */
        T_EQ(g_dn, 0);
        inject(&c, c.ps, c.ms + 3000, TF_ACK, NULL, 0, 602);
        T_EQ(g_dn, 1); T_EQ(g_dst, MT_OK);
        inject(&c, c.ps, c.ms + 3000, TF_ACK, NULL, 0, 603);   /* duplicate: no second done */
        T_EQ(g_dn, 1);
        T_EQ(r.n, 1);                             /* only ESTABLISHED: no other event */
    }
}

/* A second request is accepted while the first is in flight and continues the byte stream right
 * after it; done fires per request, A when exactly A is acked (B still pending), then B. */
static void test_send_queue_second_request_fifo(void) {
    qlog q; tcpv v;
    static uint8_t a[2000], b[700], got[4000];
    static mt_seg sa[1], sb[1];
    uint32_t i;
    memset(&q, 0, sizeof q); g_q = &q;
    for (i = 0; i < sizeof a; i++) a[i] = (uint8_t)(i + 1u);
    for (i = 0; i < sizeof b; i++) b[i] = (uint8_t)(0x80u + i);
    {
        SETUP(4096, 5000, qlog_ev, &q);
        g_pwin = 1460;
        inject(&c, c.ps, c.ms, TF_ACK, NULL, 0, 5);          /* peer window: one segment */
        q.n = 0;
        sa[0].p = a; sa[0].len = sizeof a;
        sb[0].p = b; sb[0].len = sizeof b;
        T_EQ(mt_tcp_send_segs(c.t, sa, 1, 0, 0, qlog_done, (void *)(intptr_t)1, 10), MT_OK);
        T_EQ(mt_tcp_send_segs(c.t, sb, 1, 1, 0, qlog_done, (void *)(intptr_t)2, 11), MT_OK);
        T_EQ(c.t->sq_n, 2);
        T_EQ(fl.n, 1);                                        /* window-limited: A's first 1460 */
        inject(&c, c.ps, c.ms + 1460, TF_ACK, NULL, 0, 20);
        T_EQ(fl.n, 2);                                        /* A's last 540 + all of B, pushed */
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.seq, c.ms + 1460); T_EQ(v.dlen, 1240);
        T_EQ(v.flags & TF_PSH, TF_PSH);
        T_EQ(q.n, 0);
        inject(&c, c.ps, c.ms + 1999, TF_ACK, NULL, 0, 30);   /* one byte short of A */
        T_EQ(q.n, 0);
        inject(&c, c.ps, c.ms + 2000, TF_ACK, NULL, 0, 31);   /* exactly A */
        T_EQ(q.n, 1); T_EQ(q.ord[0], 101); T_EQ(q.st[0], MT_OK);
        T_EQ(c.t->sq_n, 1); T_EQ(mt_tcp_sq_total(c.t), 700);
        inject(&c, c.ps, c.ms + 2699, TF_ACK, NULL, 0, 40);
        T_EQ(q.n, 1);
        inject(&c, c.ps, c.ms + 2700, TF_ACK, NULL, 0, 41);
        T_EQ(q.n, 2); T_EQ(q.ord[1], 102); T_EQ(q.st[1], MT_OK);
        T_EQ(wire_bytes(&fl, got, sizeof got), 2700);
        T_MEMEQ(got, a, 2000);
        T_MEMEQ(got + 2000, b, 700);
    }
}

/* A third request while two are queued is refused (MT_ERR_BUSY) without side effects; once the
 * first completes, a request queued from inside its done callback is accepted and sent. */
static void test_send_queue_third_busy(void) {
    qlog q; tcpv v;
    static const uint8_t a[100] = {1}, b[100] = {2}, d[100] = {3};
    static mt_seg sa[1], sb[1], sd[1];
    memset(&q, 0, sizeof q); g_q = &q;
    {
        SETUP(4096, 5000, qlog_ev, &q);
        q.t = c.t; q.n = 0;
        sa[0].p = a; sa[0].len = 100;
        sb[0].p = b; sb[0].len = 100;
        sd[0].p = d; sd[0].len = 100;
        T_EQ(mt_tcp_send_segs(c.t, sa, 1, 1, 0, qlog_done, (void *)(intptr_t)1, 10), MT_OK);
        T_EQ(mt_tcp_send_segs(c.t, sb, 1, 1, 0, qlog_done, (void *)(intptr_t)2, 11), MT_OK);
        T_EQ(fl.n, 2);
        fake_clear(&fl);
        T_EQ(mt_tcp_send_segs(c.t, sd, 1, 1, 0, qlog_done, (void *)(intptr_t)9, 12), MT_ERR_BUSY);
        T_EQ(fl.n, 0);                                        /* refused: nothing sent, queue intact */
        T_EQ(c.t->sq_n, 2); T_EQ(mt_tcp_sq_total(c.t), 200);
        q.queue_on_done = 1;                                  /* A's done queues request 3 */
        inject(&c, c.ps, c.ms + 100, TF_ACK, NULL, 0, 20);
        T_EQ(q.n, 1); T_EQ(q.ord[0], 101);
        T_EQ(c.t->sq_n, 2); T_EQ(mt_tcp_sq_total(c.t), 103);
        T_EQ(fl.n, 1);                                        /* request 3 went out (pushed) */
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.seq, c.ms + 200); T_MEMEQ(v.data, "ZZZ", 3);
        inject(&c, c.ps, c.ms + 203, TF_ACK, NULL, 0, 30);
        T_EQ(q.n, 3); T_EQ(q.ord[1], 102); T_EQ(q.ord[2], 103);
        T_EQ(q.st[1], MT_OK); T_EQ(q.st[2], MT_OK);
        T_EQ(c.t->sq_n, 0);
    }
}

/* One ACK completing both requests: done A then done B, before the DATA event of the same
 * segment. Then the LAST_ACK close path: the final ACK completes both requests, done fires in
 * order and TERMINATE comes last. */
static void test_done_order_matches_request_order(void) {
    qlog q;
    static const uint8_t a[10] = "0123456789", b[5] = "abcde";
    static mt_seg sa[1], sb[1];
    memset(&q, 0, sizeof q); g_q = &q;
    {
        SETUP(4096, 5000, qlog_ev, &q);
        q.n = 0;
        sa[0].p = a; sa[0].len = 10;
        sb[0].p = b; sb[0].len = 5;
        T_EQ(mt_tcp_send_segs(c.t, sa, 1, 1, 0, qlog_done, (void *)(intptr_t)1, 10), MT_OK);
        T_EQ(mt_tcp_send_segs(c.t, sb, 1, 1, 0, qlog_done, (void *)(intptr_t)2, 11), MT_OK);
        inject(&c, c.ps, c.ms + 15, TF_ACK | TF_PSH, (const uint8_t *)"r", 1, 20);
        T_EQ(q.n, 3);
        T_EQ(q.ord[0], 101); T_EQ(q.ord[1], 102); T_EQ(q.ord[2], MT_TCP_EV_DATA);
        /* close path: B is unpushed and held behind A until the close sends it with the FIN */
        T_EQ(mt_tcp_send_segs(c.t, sa, 1, 1, 0, qlog_done, (void *)(intptr_t)3, 30), MT_OK);
        T_EQ(mt_tcp_send_segs(c.t, sb, 1, 0, 0, qlog_done, (void *)(intptr_t)4, 31), MT_OK);
        inject(&c, c.ps + 1, c.ms + 15, TF_ACK | TF_FIN, NULL, 0, 32);   /* peer closes */
        T_EQ(mt_tcp_close(c.t, 33), MT_OK);                              /* LAST_ACK */
        q.n = 0;
        inject(&c, c.ps + 2, c.ms + 15 + 15 + 1, TF_ACK, NULL, 0, 40);  /* data + FIN acked */
        T_EQ(q.n, 3);
        T_EQ(q.ord[0], 103); T_EQ(q.st[0], MT_OK);
        T_EQ(q.ord[1], 104); T_EQ(q.st[1], MT_OK);
        T_EQ(q.ord[2], MT_TCP_EV_TERMINATE); T_EQ(q.st[2], MT_TERM_ULP_CLOSE);
    }
}

/* mt_tcp_abort with two requests queued (the first partly acked): both done callbacks fire with
 * MT_ERR_ABORTED, in order, then TERMINATE(ULP_ABORT); release afterwards fires nothing. */
static void test_abort_with_send_pending_fires_done_with_abort(void) {
    qlog q; tcpv v;
    static const uint8_t a[300] = {1}, b[300] = {2};
    static mt_seg sa[1], sb[1];
    memset(&q, 0, sizeof q); g_q = &q;
    {
        SETUP(4096, 5000, qlog_ev, &q);
        q.n = 0;
        sa[0].p = a; sa[0].len = 300;
        sb[0].p = b; sb[0].len = 300;
        T_EQ(mt_tcp_send_segs(c.t, sa, 1, 1, 0, qlog_done, (void *)(intptr_t)1, 10), MT_OK);
        T_EQ(mt_tcp_send_segs(c.t, sb, 1, 1, 0, qlog_done, (void *)(intptr_t)2, 11), MT_OK);
        inject(&c, c.ps, c.ms + 100, TF_ACK, NULL, 0, 12);    /* A partly acked */
        T_EQ(q.n, 0);
        fake_clear(&fl);
        mt_tcp_abort(c.t, 20);
        T_EQ(q.n, 3);
        T_EQ(q.ord[0], 101); T_EQ(q.st[0], MT_ERR_ABORTED);
        T_EQ(q.ord[1], 102); T_EQ(q.st[1], MT_ERR_ABORTED);
        T_EQ(q.ord[2], MT_TCP_EV_TERMINATE); T_EQ(q.st[2], MT_TERM_ULP_ABORT);
        T_EQ(fl.n, 1);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.flags & TF_RST, TF_RST);
        T_EQ(c.t->sq_n, 0);
        mt_tcp_release(c.t);
        T_EQ(q.n, 3);                                         /* nothing more */
    }
}

static void test_send_zero_len_seg_skipped(void) {
    rec r; tcpv v;
    static const uint8_t x[3] = {'x', 'y', 'z'}, w[2] = {'w', 'v'};
    static mt_seg sg[6], zero[2], nul[1], nine[MT_SEGS_MAX + 1];
    int i;
    memset(&r, 0, sizeof r);
    sg[0].p = x; sg[0].len = 0;                   /* leading empty entry (cursor starts on it) */
    sg[1].p = x; sg[1].len = 3;
    sg[2].p = w; sg[2].len = 0;
    sg[3].p = NULL; sg[3].len = 0;
    sg[4].p = w; sg[4].len = 2;
    sg[5].p = x; sg[5].len = 0;                   /* trailing empty entry */
    zero[0].p = x; zero[0].len = 0; zero[1].p = NULL; zero[1].len = 0;
    nul[0].p = NULL; nul[0].len = 4;
    for (i = 0; i <= MT_SEGS_MAX; i++) { nine[i].p = x; nine[i].len = 1; }
    {
        SETUP(4096, 5000, rec_cb, &r);
        T_EQ(mt_tcp_send_segs(c.t, zero, 2, 1, 0, NULL, NULL, 5), MT_ERR_INVAL);   /* 0 bytes */
        T_EQ(mt_tcp_send_segs(c.t, sg, 0, 1, 0, NULL, NULL, 5), MT_ERR_INVAL);
        T_EQ(mt_tcp_send_segs(c.t, NULL, 1, 1, 0, NULL, NULL, 5), MT_ERR_INVAL);
        T_EQ(mt_tcp_send_segs(c.t, nul, 1, 1, 0, NULL, NULL, 5), MT_ERR_INVAL);
        T_EQ(mt_tcp_send_segs(c.t, nine, MT_SEGS_MAX + 1, 1, 0, NULL, NULL, 5), MT_ERR_INVAL);
        T_EQ(fl.n, 0); T_EQ(c.t->sq_n, 0);
        T_EQ(mt_tcp_send_segs(c.t, nine, MT_SEGS_MAX, 1, 0, NULL, NULL, 5), MT_OK);   /* 8 is fine */
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.dlen, MT_SEGS_MAX); T_MEMEQ(v.data, "xxxxxxxx", 8);
        inject(&c, c.ps, c.ms + MT_SEGS_MAX, TF_ACK, NULL, 0, 6);
        fake_clear(&fl);
        T_EQ(mt_tcp_send_segs(c.t, sg, 6, 1, 0, NULL, NULL, 10), MT_OK);
        T_EQ(fl.n, 1);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.dlen, 5); T_MEMEQ(v.data, "xyzwv", 5);
        T_CHECK(v.checksum_ok);
        inject(&c, c.ps, c.ms + MT_SEGS_MAX + 4, TF_ACK, NULL, 0, 20);   /* cursor across empties */
        T_EQ(c.t->sq_idx, 4); T_EQ(c.t->sq_off, 1);
        mt_tick(c.s, 20 + 1000);                                         /* RTO: resend the tail */
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.dlen, 1); T_EQ(v.data[0], 'v');
        T_EQ(v.seq, c.ms + MT_SEGS_MAX + 4);
        inject(&c, c.ps, c.ms + MT_SEGS_MAX + 5, TF_ACK, NULL, 0, 1100);
        T_EQ(c.t->sq_n, 0);
    }
}

/* The whole mt_tcp_create buffer is the receive ring: a 4096-byte buffer advertises 4096 (in
 * the SYN and once established). The core minimum is 512 bytes. */
static void test_recv_window_equals_buflen(void) {
    rec r; tcpv v; mt_tcp_info in;
    static fake_link fl2, fl3; static uint8_t b2[600], b3[4096];
    mt_stack *s2, *s3; mt_tcp *t3;
    memset(&r, 0, sizeof r);
    s2 = tstack_new(&fl2, T_ME);
    T_CHECK(mt_tcp_create(s2, b2, 511, rec_cb, &r) == NULL);
    T_CHECK(mt_tcp_create(s2, b2, 512, rec_cb, &r) != NULL);
    s3 = tstack_new(&fl3, T_ME);
    mt_arp_insert(s3, T_PEER, T_PEER_MAC, 0);
    t3 = mt_tcp_create(s3, b3, sizeof b3, rec_cb, &r);
    T_EQ(mt_tcp_open(t3, T_PEER, 23, 0, 0), MT_OK);
    T_CHECK(view_tcp(fl3.f[0], fl3.len[0], &v)); T_EQ(v.win, 4096);
    {
        SETUP(4096, 5000, rec_cb, &r);
        mt_tcp_get_info(c.t, &in); T_EQ(in.rcv_wnd, 4096);
        T_EQ(tsend(c.t, (const uint8_t *)"j", 1, 1, 0, 10), 1);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.win, 4096);       /* advertised window == buflen */
        inject_data(&c, 0, 1000, TF_PSH, 20);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.win, 3096);       /* 4096 - 1000, no carve-out */
    }
}

/* Push is per request: a sub-MSS tail goes out while data is in flight only when the segment
 * reaches the end of a pushed request; a later unpushed request is held by Nagle. */
static void test_push_per_request(void) {
    rec r; tcpv v;
    static uint8_t a[1560], b[50];
    memset(&r, 0, sizeof r);
    memset(a, 'a', sizeof a); memset(b, 'b', sizeof b);
    {
        SETUP(4096, 5000, rec_cb, &r);
        g_pwin = 1460;
        inject(&c, c.ps, c.ms, TF_ACK, NULL, 0, 5);                  /* window: one segment */
        T_EQ(tsend(c.t, a, 1560, 0, 0, 10), 1560);                   /* unpushed */
        T_EQ(fl.n, 1);                                               /* 1460 (window-limited) */
        inject(&c, c.ps, c.ms + 1000, TF_ACK, NULL, 0, 11);          /* 460 still in flight */
        T_EQ(fl.n, 1);                                               /* A's 100-byte tail held */
        T_EQ(tsend(c.t, b, 50, 1, 0, 12), 50);                       /* pushed B releases it */
        T_EQ(fl.n, 2);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.dlen, 150); T_EQ(v.seq, c.ms + 1460);
        T_EQ(v.flags & TF_PSH, TF_PSH);
        inject(&c, c.ps, c.ms + 1610, TF_ACK, NULL, 0, 20);
        T_EQ(c.t->sq_n, 0);
        T_EQ(tsend(c.t, a, 100, 1, 0, 21), 100);                     /* idle: goes */
        T_EQ(fl.n, 3);
        T_EQ(tsend(c.t, b, 50, 0, 0, 22), 50);                       /* unpushed behind it: held */
        T_EQ(fl.n, 3);
        inject(&c, c.ps, c.ms + 1710, TF_ACK, NULL, 0, 30);          /* ACK: the held 50 go */
        T_EQ(fl.n, 4);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.dlen, 50); T_EQ(v.seq, c.ms + 1710);
    }
}

T_MAIN(test_sws_no_small_window_updates, test_delayed_ack_carried_by_send,
       test_psh_full_segment_acked_now, test_send_state_and_no_events_after_terminate,
       test_data_with_fin_single_segment, test_nagle_holds_tail_in_flight,
       test_gap_acks_every_fill_segment, test_gap_high_mark_wraps, test_send_segs_no_copy,
       test_send_segs_retransmit_from_caller, test_send_done_fires_on_ack_only,
       test_send_queue_second_request_fifo, test_send_queue_third_busy,
       test_done_order_matches_request_order, test_abort_with_send_pending_fires_done_with_abort,
       test_send_zero_len_seg_skipped, test_recv_window_equals_buflen, test_push_per_request)
