#include "t.h"
#include "net.h"
#include "mt_int.h"
#include "mt_util.h"

/* TCP data transfer, part 2: receiver SWS, delayed-ACK cancellation, PSH on full segments,
 * send state errors, events after TERMINATE, data+FIN. Every test uses its own connection (established at now=0, peer MSS 1460,
 * peer window 8192, our rings split buflen/4 : rest). */

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
        SETUP(4096, 5000, rec_cb, &r);                  /* receive ring 3072, threshold 1460 */
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
        T_EQ(mt_tcp_send(c.t, (const uint8_t *)"q", 1, 1, 0, 1020), 1);
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

static void test_send_state_and_no_events_after_terminate(void) {
    rec r; static uint8_t d[2000];
    memset(&r, 0, sizeof r);
    {
        SETUP(4096, 5000, rec_cb, &r);
        T_EQ(mt_tcp_send(c.t, d, 2000, 0, 0, 10), 1024);   /* short: SEND_SPACE owed */
        inject(&c, c.ps, c.ms + 1024, TF_RST | TF_ACK, NULL, 0, 20);
        T_EQ(count_ev(&r, MT_TCP_EV_TERMINATE), 1);
        T_EQ(count_ev(&r, MT_TCP_EV_SEND_SPACE), 0);
        T_EQ(mt_tcp_send(c.t, d, 10, 1, 0, 21), MT_ERR_STATE);
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
        T_EQ(mt_tcp_send(c.t, (const uint8_t *)"ok", 2, 1, 0, 12), 2);   /* half close: still sends */
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.dlen, 2);
    }
}

static void test_nagle_holds_tail_in_flight(void) {
    rec r; tcpv v; static uint8_t d[1560];
    memset(&r, 0, sizeof r);
    pattern(d, 0, sizeof d);
    {
        SETUP(8192, 5000, rec_cb, &r);
        T_EQ(mt_tcp_send(c.t, (const uint8_t *)"x", 1, 1, 0, 10), 1);
        T_EQ(fl.n, 1);                                  /* 1 byte now in flight */
        T_EQ(mt_tcp_send(c.t, d, 1560, 0, 0, 11), 1560);
        T_EQ(fl.n, 2);                                  /* one full segment only */
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.dlen, 1460);
        inject(&c, c.ps, c.ms + 1, TF_ACK, NULL, 0, 12);   /* ACKs x only: 1460 still in flight */
        T_EQ(fl.n, 2);                                  /* 100-byte tail still held */
        inject(&c, c.ps, c.ms + 1461, TF_ACK, NULL, 0, 13);
        T_EQ(fl.n, 3);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.dlen, 100); T_EQ(v.seq, c.ms + 1461);
        /* with push the tail goes at once, data still in flight */
        T_EQ(mt_tcp_send(c.t, d, 1560, 1, 0, 20), 1560);
        T_EQ(fl.n, 5);
        T_CHECK(last_tcp(&fl, &v)); T_EQ(v.dlen, 100);
    }
}

T_MAIN(test_sws_no_small_window_updates, test_delayed_ack_carried_by_send,
       test_psh_full_segment_acked_now, test_send_state_and_no_events_after_terminate,
       test_data_with_fin_single_segment, test_nagle_holds_tail_in_flight)
