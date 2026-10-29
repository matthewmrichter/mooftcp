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

static void inject(mt_stack *s, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                   uint8_t flags, const uint8_t *data, uint16_t dlen, uint32_t now) {
    static uint8_t buf[1700];
    uint8_t *f = buf + 1;
    uint16_t n = mk_tcp(f, T_PEER, sport, T_ME, dport, seq, ack, flags, 8192,
                        (flags & TF_SYN) ? MSS1460 : NULL, (flags & TF_SYN) ? 4 : 0, data, dlen);
    mt_input(s, f, n, now);
}

static int last_tcp(const fake_link *fl, tcpv *v) {
    return fl->n > 0 && view_tcp(fl->f[fl->n - 1], fl->len[fl->n - 1], v);
}

/* ---- simultaneous open ---- */

static void test_simultaneous_open_scripted(void) {
    fake_link fl; rec r; tcpv v; mt_tcp_info in;
    mt_stack *s = setup(&fl);
    static uint8_t b[8192];
    mt_tcp *t = mt_tcp_create(s, b, sizeof b, rec_cb, &r);
    memset(&r, 0, sizeof r);
    T_EQ(mt_tcp_open(t, T_PEER, 2000, 2000, 0), MT_OK);   /* iss = 1 */
    inject(s, 2000, 2000, 9000, 0, TF_SYN, NULL, 0, 1);    /* bare SYN */
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_SYN_RCVD);
    T_EQ(fl.n, 2);
    T_CHECK(last_tcp(&fl, &v)); T_EQ(v.flags, TF_SYN | TF_ACK); T_EQ(v.seq, 1); T_EQ(v.ack, 9001);
    inject(s, 2000, 2000, 9000, 2, TF_SYN | TF_ACK, NULL, 0, 2);   /* peer's SYN-ACK */
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_ESTABLISHED);
    T_EQ(fl.n, 3);                                  /* exactly one ACK, no loop */
    T_CHECK(last_tcp(&fl, &v)); T_EQ(v.flags, TF_ACK); T_EQ(v.seq, 2); T_EQ(v.ack, 9001);
    T_EQ(r.n, 1); T_EQ(r.e[0].ev, MT_TCP_EV_ESTABLISHED);
    inject(s, 2000, 2000, 9000, 0, TF_SYN, NULL, 0, 3);    /* stray bare SYN now: challenge ACK */
    T_EQ(fl.n, 4);
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_ESTABLISHED);
}

static void pump(fake_link *a, mt_stack *sa, fake_link *b, mt_stack *sb, int *total) {
    int round, i;
    for (round = 0; round < 12; round++) {
        int moved = 0;
        for (i = 0; i < a->n; i++) { mt_input(sb, a->f[i], a->len[i], 1); moved++; (*total)++; }
        fake_clear(a);
        for (i = 0; i < b->n; i++) { mt_input(sa, b->f[i], b->len[i], 1); moved++; (*total)++; }
        fake_clear(b);
        if (!moved) break;
    }
}

static void test_simultaneous_open_two_stacks(void) {
    fake_link fa, fb; rec ra, rb; mt_tcp_info ia, ib; int total = 0;
    mt_stack *sa = tstack_new(&fa, T_ME), *sb = tstack_new(&fb, T_PEER);
    static uint8_t ba[8192], bb[8192];
    mt_tcp *ta = mt_tcp_create(sa, ba, sizeof ba, rec_cb, &ra);
    mt_tcp *tb = mt_tcp_create(sb, bb, sizeof bb, rec_cb, &rb);
    memset(&ra, 0, sizeof ra); memset(&rb, 0, sizeof rb);
    mt_arp_insert(sa, T_PEER, T_ME_MAC, 0);   /* both stacks share one MAC */
    mt_arp_insert(sb, T_ME, T_ME_MAC, 0);
    T_EQ(mt_tcp_open(ta, T_PEER, 2000, 2000, 0), MT_OK);
    T_EQ(mt_tcp_open(tb, T_ME, 2000, 2000, 0), MT_OK);
    pump(&fa, sa, &fb, sb, &total);
    mt_tcp_get_info(ta, &ia); mt_tcp_get_info(tb, &ib);
    T_EQ(ia.state, MT_TCP_ESTABLISHED); T_EQ(ib.state, MT_TCP_ESTABLISHED);
    T_CHECK(total <= 12);
    T_EQ(ra.n, 1); T_EQ(rb.n, 1);
}

/* ---- callbacks vs re-entrant API calls ---- */

typedef struct {
    rec r, r2;
    int abort_on_data, relisten, release_on_data;
    mt_tcp *newt;
    uint8_t *buf2;
} cbx;

static void cbx_fn(mt_tcp *t, mt_tcp_event ev, uint32_t arg, void *user) {
    cbx *c = (cbx *)user;
    rec_cb(t, ev, arg, &c->r);
    if (ev == MT_TCP_EV_DATA && c->abort_on_data) mt_tcp_abort(t, 10);
    if (ev == MT_TCP_EV_DATA && c->release_on_data) {
        mt_stack *st = t->st;
        mt_tcp_release(t);
        c->newt = mt_tcp_create(st, c->buf2, 4096, rec_cb, &c->r2);
    }
    if (ev == MT_TCP_EV_TERMINATE && c->relisten) mt_tcp_listen(t, 0, 0, 21);
}

static int count_ev(const rec *r, int ev) {
    int i, n = 0;
    for (i = 0; i < r->n && i < 32; i++) n += r->e[i].ev == ev;
    return n;
}

static mt_tcp *est_with_cb(mt_stack *s, fake_link *fl, cbx *c, uint32_t *ps, uint32_t *ms) {
    mt_tcp *t = tconn_established(s, fl, &c->r, ps, ms);
    t->cb = cbx_fn;
    t->user = c;
    return t;
}

static void test_abort_inside_data_callback(void) {
    fake_link fl; cbx c; mt_tcp_info in; uint32_t ps, ms;
    mt_stack *s = setup(&fl);
    mt_tcp *t;
    memset(&c, 0, sizeof c);
    t = est_with_cb(s, &fl, &c, &ps, &ms);
    c.abort_on_data = 1;
    mt_tcp_get_info(t, &in);
    inject(s, 23, in.lport, ps, ms, TF_ACK | TF_FIN, (const uint8_t *)"hi", 2, 10);
    T_EQ(count_ev(&c.r, MT_TCP_EV_DATA), 1);
    T_EQ(count_ev(&c.r, MT_TCP_EV_TERMINATE), 1);
    T_EQ(count_ev(&c.r, MT_TCP_EV_CLOSING), 0);
    T_EQ(c.r.n, 3);                                 /* ESTABLISHED, DATA, TERMINATE */
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_CLOSED);
}

static void test_relisten_in_terminate_callback(void) {
    fake_link fl; cbx c; mt_tcp_info in; uint32_t ps, ms;
    mt_stack *s = setup(&fl);
    mt_tcp *t;
    memset(&c, 0, sizeof c);
    t = est_with_cb(s, &fl, &c, &ps, &ms);
    c.abort_on_data = 1; c.relisten = 1;
    mt_tcp_get_info(t, &in);
    inject(s, 23, in.lport, ps, ms, TF_ACK | TF_FIN, (const uint8_t *)"hi", 2, 10);
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_LISTEN);
    T_EQ(c.r.n, 3);                                 /* no stale CLOSING on the new listener */
    T_EQ(count_ev(&c.r, MT_TCP_EV_CLOSING), 0);
}

static void test_release_create_in_callback(void) {
    fake_link fl; cbx c; mt_tcp_info in; uint32_t ps, ms;
    static uint8_t b2[4096];
    mt_stack *s = setup(&fl);
    mt_tcp *t;
    memset(&c, 0, sizeof c);
    t = est_with_cb(s, &fl, &c, &ps, &ms);
    c.release_on_data = 1; c.buf2 = b2;
    mt_tcp_get_info(t, &in);
    inject(s, 23, in.lport, ps, ms, TF_ACK | TF_FIN, (const uint8_t *)"hi", 2, 10);
    T_CHECK(c.newt == t);                           /* the slot really was reused */
    T_EQ(c.r2.n, 0);                                /* and saw nothing meant for the old TCB */
    T_EQ(count_ev(&c.r, MT_TCP_EV_CLOSING), 0);
}

/* ---- zero receive window ---- */

static void test_zero_window_still_processes_ack(void) {
    fake_link fl; rec r; tcpv v; mt_tcp_info in; uint32_t ps, ms;
    static uint8_t junk[8192];
    mt_stack *s = setup(&fl);
    mt_tcp *t;
    memset(&r, 0, sizeof r);
    t = tconn_established(s, &fl, &r, &ps, &ms);
    mt_tcp_get_info(t, &in);
    T_EQ(mt_ring_write(&t->sndq, (const uint8_t *)"AAAA", 4), 4);
    mt_tcp_output(t, 5);
    T_EQ(fl.n, 1);
    T_EQ(mt_ring_write(&t->rcvq, junk, mt_ring_free(&t->rcvq)), in.rcv_wnd);
    mt_tcp_get_info(t, &in); T_EQ(in.rcv_wnd, 0); T_EQ(in.unacked, 4);
    inject(s, 23, in.lport, ps, ms + 4, TF_ACK, (const uint8_t *)"xx", 2, 6);
    mt_tcp_get_info(t, &in);
    T_EQ(in.unacked, 0);                            /* the ACK counted */
    T_EQ(in.rcv_avail, 6144);                       /* the data did not */
    T_EQ(r.n, 1);                                   /* no DATA event */
    T_EQ(fl.n, 2);
    T_CHECK(last_tcp(&fl, &v)); T_EQ(v.flags, TF_ACK); T_EQ(v.win, 0); T_EQ(v.ack, ps);
}

/* ---- degenerate MTU ---- */

static void mtu_case(uint16_t mtu) {
    fake_link fl; rec r; tcpv v; mt_tcp_info in; mt_ifconfig c; uint8_t d[1000];
    static uint8_t b[8192];
    mt_stack *s = setup(&fl);
    mt_tcp *t;
    memset(&r, 0, sizeof r);
    memset(d, 7, sizeof d);
    mt_get_config(s, &c); c.mtu = mtu; mt_set_config(s, &c);
    mt_get_config(s, &c); T_CHECK(c.mtu >= 68 && c.mtu <= 1500);
    t = mt_tcp_create(s, b, sizeof b, rec_cb, &r);
    T_EQ(mt_tcp_open(t, T_PEER, 23, 0, 0), MT_OK);
    T_CHECK(last_tcp(&fl, &v)); T_EQ(v.mss, 64);
    inject(s, 23, v.sport, 5000, v.seq + 1, TF_SYN | TF_ACK, NULL, 0, 1);
    T_EQ(t->mss, 64);
    fake_clear(&fl);
    T_EQ(mt_ring_write(&t->sndq, d, sizeof d), sizeof d);
    mt_tcp_output(t, 2);                            /* must return */
    T_CHECK(fl.n > 0 && fl.n <= 40);
    mt_tcp_get_info(t, &in); T_EQ(in.unacked + in.send_queued, 1000);
}

static void test_tiny_mtu_clamped(void) {
    mtu_case(0);
    mtu_case(40);
}

T_MAIN(test_simultaneous_open_scripted, test_simultaneous_open_two_stacks,
       test_abort_inside_data_callback, test_relisten_in_terminate_callback,
       test_release_create_in_callback, test_zero_window_still_processes_ack,
       test_tiny_mtu_clamped)
