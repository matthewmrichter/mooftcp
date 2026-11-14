#include "t.h"
#include "net.h"
#include "mt_int.h"
#include "mt_util.h"
#include "pbfake.h"
#include "tune_parse.h"
#ifdef MT_TUNE
#include "mt_tune_abi.h"
#endif

/* MT_TUNE (perf builds only): the run-time timing levers. Built and run only with -DMT_TUNE=ON (build-tune);
 * the release configurations leave this file out (CMakeLists.txt). Each connection is established at now=0
 * (peer MSS 1460, window 8192; the whole buffer is the receive ring). */

#ifdef MT_TUNE

static const uint8_t MSS1460[4] = {2, 4, 0x05, 0xB4};

typedef struct { mt_stack *s; fake_link *fl; mt_tcp *t; uint16_t lport; uint32_t ps, ms; } C;

static void inject(C *c, uint32_t seq, uint32_t ack, uint8_t flags, const uint8_t *d, uint16_t n, uint32_t now) {
    static uint8_t buf[1700];
    uint8_t *f = buf + 1;
    uint16_t len = mk_tcp(f, T_PEER, 23, T_ME, c->lport, seq, ack, flags, 8192, NULL, 0, d, n);
    mt_input(c->s, f, len, now);
}

static int open_conn(C *c, fake_link *fl, uint8_t *buf, uint32_t buflen, const mt_tune *tn) {
    tcpv v;
    uint8_t f[200];
    uint16_t n;
    c->fl = fl;
    c->s = tstack_new(fl, T_ME);
    if (tn && mt_tune_set(c->s, tn) != MT_OK) return 0;
    mt_arp_insert(c->s, T_PEER, T_PEER_MAC, 0);
    c->t = mt_tcp_create(c->s, buf, buflen, NULL, NULL);
    if (!c->t || mt_tcp_open(c->t, T_PEER, 23, 0, 0) != MT_OK) return 0;
    if (fl->n != 1 || !view_tcp(fl->f[0], fl->len[0], &v)) return 0;
    c->lport = v.sport;
    n = mk_tcp(f, T_PEER, 23, T_ME, v.sport, 5000, v.seq + 1, TF_SYN | TF_ACK, 8192, MSS1460, 4, NULL, 0);
    mt_input(c->s, f, n, 0);
    c->ps = 5001;
    c->ms = v.seq + 1;
    fake_clear(fl);
    return 1;
}

#define SETUP(tn) \
    static fake_link fl; static uint8_t buf[16384]; static uint8_t dat[1500]; C c; \
    memset(&c, 0, sizeof c); memset(dat, 0x5A, sizeof dat); \
    T_CHECK(open_conn(&c, &fl, buf + 1, 8192, (tn)))

static mt_tune mk(uint16_t rmin, uint16_t rinit, uint16_t da, uint16_t st, uint8_t q) {
    mt_tune t;
    t.rto_min = rmin; t.rto_init = rinit; t.delack_ms = da; t.stall_ms = st; t.quickack = q;
    return t;
}

static void test_defaults_equal_defines(void) {
    fake_link fl; mt_stack *s; mt_tune t;
    memset(&fl, 0, sizeof fl);
    s = tstack_new(&fl, T_ME);
    mt_tune_get(s, &t);
    T_EQ(t.rto_min, MT_TCP_RTO_MIN);
    T_EQ(t.rto_init, MT_TCP_RTO_INIT);
    T_EQ(t.delack_ms, MT_TCP_DELACK_MS);
    T_EQ(t.stall_ms, MT_TCP_STALL_MS);
    T_EQ(t.quickack, MT_TCP_QUICKACK);
    T_EQ(MT_RTO_MIN(s), MT_TCP_RTO_MIN);
    T_EQ(MT_QUICKACK(s), MT_TCP_QUICKACK);
}

static void test_set_get_round_trip(void) {
    fake_link fl; mt_stack *s; mt_tune t, g, ok = mk(300, 700, 20, 5, 16);
    memset(&fl, 0, sizeof fl);
    s = tstack_new(&fl, T_ME);
    T_EQ(mt_tune_set(s, &ok), MT_OK);
    mt_tune_get(s, &g);
    T_EQ(g.rto_min, 300); T_EQ(g.rto_init, 700); T_EQ(g.delack_ms, 20); T_EQ(g.stall_ms, 5); T_EQ(g.quickack, 16);
    t = mk(100, 100, 0, 1, 0);                              /* the lower bounds are accepted */
    T_EQ(mt_tune_set(s, &t), MT_OK);
    t = mk(5000, 10000, 500, 100, 255);                     /* and the upper ones */
    T_EQ(mt_tune_set(s, &t), MT_OK);
    mt_tune_get(s, &g);
    T_EQ(g.rto_min, 5000); T_EQ(g.rto_init, 10000); T_EQ(g.delack_ms, 500); T_EQ(g.stall_ms, 100); T_EQ(g.quickack, 255);
}

static void test_each_bound_refused(void) {
    fake_link fl; mt_stack *s; mt_tune before, bad[8];
    int i;
    memset(&fl, 0, sizeof fl);
    s = tstack_new(&fl, T_ME);
    bad[0] = mk(99, 1000, 100, 10, 4);
    bad[1] = mk(5001, 6000, 100, 10, 4);
    bad[2] = mk(500, 499, 100, 10, 4);                      /* rto_init < rto_min */
    bad[3] = mk(500, 10001, 100, 10, 4);
    bad[4] = mk(1000, 1000, 501, 10, 4);
    bad[5] = mk(1000, 1000, 100, 0, 4);
    bad[6] = mk(1000, 1000, 100, 101, 4);
    bad[7] = mk(0, 0, 0, 0, 0);
    mt_tune_get(s, &before);
    for (i = 0; i < 8; i++) T_EQ(mt_tune_set(s, &bad[i]), MT_ERR_INVAL);
    T_EQ(mt_tune_set(s, NULL), MT_ERR_INVAL);
    {
        mt_tune after;
        mt_tune_get(s, &after);
        T_MEMEQ(&after, &before, sizeof after);             /* a refused set changes nothing */
    }
}

/* delack 0 (quickack 0 so the first segment is not ACKed by the quick phase): a full-size segment is ACKed
 * before mt_input returns, with no delayed-ACK timer. Default delack: the same segment waits. */
static void test_delack0_acks_first_segment_at_once(void) {
    tcpv v;
    mt_tune t = mk(1000, 1000, 0, 10, 0);
    {
        SETUP(&t);
        inject(&c, c.ps, c.ms, TF_ACK, dat, 1460, 10);
        T_EQ(fl.n, 1);
        T_CHECK(view_tcp(fl.f[0], fl.len[0], &v)); T_EQ(v.ack, c.ps + 1460u);
        T_CHECK(!(c.t->tmask & (1u << MT_TT_DELACK)));
    }
    t = mk(1000, 1000, 100, 10, 0);
    {
        SETUP(&t);
        inject(&c, c.ps, c.ms, TF_ACK, dat, 1460, 10);
        T_EQ(fl.n, 0);                                      /* delayed */
        T_EQ(mt_next_deadline(c.s, 10), 100);
    }
}

static void test_delack_time_is_used(void) {
    mt_tune t = mk(1000, 1000, 40, 10, 0);
    SETUP(&t);
    inject(&c, c.ps, c.ms, TF_ACK, dat, 1460, 10);
    T_EQ(fl.n, 0);
    T_EQ(mt_next_deadline(c.s, 10), 40);
    mt_tick(c.s, 49); T_EQ(fl.n, 0);
    mt_tick(c.s, 50); T_EQ(fl.n, 1);
}

static void test_quickack_255_never_arms_delack(void) {
    mt_tune t = mk(1000, 1000, 100, 10, 255);
    int i;
    SETUP(&t);
    /* 300 small segments, past the 255 of the default counter width: each is ACKed before mt_input returns */
    for (i = 0; i < 300; i++) {
        uint32_t off = (uint32_t)i * 100u;
        fake_clear(&fl);
        inject(&c, c.ps + off, c.ms, TF_ACK, dat, 100, 10 + (uint32_t)i);
        T_EQ(fl.n, 1);
        T_CHECK(!(c.t->tmask & (1u << MT_TT_DELACK)));
        mt_tcp_consume(c.t, 100, 10 + (uint32_t)i);
    }
}

static void test_quickack_n_counts(void) {
    mt_tune t = mk(1000, 1000, 100, 10, 2);
    SETUP(&t);
    inject(&c, c.ps, c.ms, TF_ACK, dat, 1460, 10);
    T_EQ(fl.n, 1);
    inject(&c, c.ps + 1460u, c.ms, TF_ACK, dat, 1460, 11);
    T_EQ(fl.n, 2);
    inject(&c, c.ps + 2920u, c.ms, TF_ACK, dat, 1460, 12);   /* third: delayed */
    T_EQ(fl.n, 2);
    T_EQ(mt_next_deadline(c.s, 12), 100);
}

/* rto_min/rto_init 500: a segment on a silent peer is retransmitted at 500 ms, not 1000. */
static void test_rto_500_fires_at_500(void) {
    mt_tune t = mk(500, 500, 100, 10, 4);
    SETUP(&t);
    T_EQ(tsend(c.t, dat, 100, 1, 0, 10), 100);
    T_EQ(fl.n, 1);
    T_EQ(mt_next_deadline(c.s, 10), 500);
    mt_tick(c.s, 509); T_EQ(fl.n, 1);
    mt_tick(c.s, 510); T_EQ(fl.n, 2);                       /* retransmitted */
    T_EQ(mt_next_deadline(c.s, 510), 1000);                 /* backed off from 500 */
}

/* The floor applies to measured RTOs: a 50 ms RTT sample gives rto = rto_min (500), not 1000. */
static void test_rto_min_floor_after_sample(void) {
    mt_tune t = mk(500, 500, 100, 10, 4);
    mt_tcp_info in;
    SETUP(&t);
    tsend(c.t, dat, 100, 1, 0, 100);
    inject(&c, c.ps, c.ms + 101u, TF_ACK, NULL, 0, 150);
    mt_tcp_get_info(c.t, &in);
    T_EQ(in.rto_ms, 500);
}

static void test_stall_ms_used(void) {
    mt_tune t = mk(1000, 1000, 100, 5, 4);
    SETUP(&t);
    fl.refuse = 1;
    T_EQ(tsend(c.t, dat, 100, 1, 0, 30), 100);
    T_EQ(fl.n, 0);
    T_EQ(mt_next_deadline(c.s, 30), 5);
    mt_tick(c.s, 35);
    T_EQ(fl.n, 1);
}

/* Running timers keep their deadline: a delayed ACK armed under 100 ms still fires then after the lever moves. */
static void test_running_timer_keeps_deadline(void) {
    mt_tune t = mk(1000, 1000, 100, 10, 0), n = mk(1000, 1000, 20, 10, 0);
    SETUP(&t);
    inject(&c, c.ps, c.ms, TF_ACK, dat, 1460, 10);
    T_EQ(mt_next_deadline(c.s, 10), 100);
    T_EQ(mt_tune_set(c.s, &n), MT_OK);
    T_EQ(mt_next_deadline(c.s, 10), 100);
}

/* The private Control call through the PB dispatcher: get, set, refusals; unconfigured driver answers -28. */
static int16_t tune_call(pbt *x, uint16_t *blk, uint16_t ver, uint16_t op) {
    TCPiopb *pb = pbt_pb(MT_CS_TUNE, 0);
    uint16_t *b = (uint16_t *)pbf_alloc(MT_TUNE_BLOCK_BYTES);
    MacPtr32 a = pbf_a(b);
    memcpy(b, blk, MT_TUNE_BLOCK_BYTES);
    b[MT_TW_VERSION] = ver;
    b[MT_TW_OP] = op;
    memcpy((uint8_t *)pb + 28, &a, 4);
    {
        int16_t r = pbt_call(x, pb);
        memcpy(blk, b, MT_TUNE_BLOCK_BYTES);
        return r;
    }
}

static void test_pb_control_get_set(void) {
    pbt x;
    uint16_t w[MT_TW_N];
    mt_tune t;
    pbt_init(&x, 1);
    memset(w, 0, sizeof w);
    T_EQ(tune_call(&x, w, MT_TUNE_ABI_VERSION, MT_TUNE_OP_GET), 0);
    T_EQ(w[MT_TW_RTO_MIN], MT_TCP_RTO_MIN); T_EQ(w[MT_TW_RTO_INIT], MT_TCP_RTO_INIT);
    T_EQ(w[MT_TW_DELACK], MT_TCP_DELACK_MS); T_EQ(w[MT_TW_STALL], MT_TCP_STALL_MS); T_EQ(w[MT_TW_QUICKACK], MT_TCP_QUICKACK);
    w[MT_TW_RTO_MIN] = 500; w[MT_TW_RTO_INIT] = 700; w[MT_TW_DELACK] = 0; w[MT_TW_STALL] = 5; w[MT_TW_QUICKACK] = 255;
    T_EQ(tune_call(&x, w, MT_TUNE_ABI_VERSION, MT_TUNE_OP_SET), 0);
    mt_tune_get(x.d->s, &t);
    T_EQ(t.rto_min, 500); T_EQ(t.rto_init, 700); T_EQ(t.delack_ms, 0); T_EQ(t.stall_ms, 5); T_EQ(t.quickack, 255);
    w[MT_TW_STALL] = 0;                                                   /* out of range: refused, unchanged */
    T_EQ(tune_call(&x, w, MT_TUNE_ABI_VERSION, MT_TUNE_OP_SET), MT_TUNE_PARAMERR);
    mt_tune_get(x.d->s, &t); T_EQ(t.stall_ms, 5);
    w[MT_TW_QUICKACK] = 256;
    T_EQ(tune_call(&x, w, MT_TUNE_ABI_VERSION, MT_TUNE_OP_SET), MT_TUNE_PARAMERR);
    T_EQ(tune_call(&x, w, 3, MT_TUNE_OP_GET), MT_TUNE_PARAMERR);          /* bad version */
    T_EQ(tune_call(&x, w, MT_TUNE_ABI_VERSION, 7), MT_TUNE_PARAMERR);     /* bad op */
    T_EQ(pbf_bad, 0);
}

/* MoofTCPTune's presets through the PB call: delack0 then default (what the app sends for those run ids) leaves the
 * stack at the release constants again. */
static void test_presets_apply_and_default_restores(void) {
    pbt x;
    uint16_t w[MT_TW_N];
    tn_vals v;
    mt_tune t;
    pbt_init(&x, 1);
    memset(w, 0, sizeof w);
    T_CHECK(tn_preset("delack0", &v));
    w[MT_TW_RTO_MIN] = (uint16_t)v.rto_min; w[MT_TW_RTO_INIT] = (uint16_t)v.rto_init; w[MT_TW_DELACK] = (uint16_t)v.delack;
    w[MT_TW_STALL] = (uint16_t)v.stall; w[MT_TW_QUICKACK] = (uint16_t)v.quickack;
    T_EQ(tune_call(&x, w, MT_TUNE_ABI_VERSION, MT_TUNE_OP_SET), 0);
    mt_tune_get(x.d->s, &t); T_EQ(t.delack_ms, 0);
    T_CHECK(tn_preset("default", &v));
    w[MT_TW_RTO_MIN] = (uint16_t)v.rto_min; w[MT_TW_RTO_INIT] = (uint16_t)v.rto_init; w[MT_TW_DELACK] = (uint16_t)v.delack;
    w[MT_TW_STALL] = (uint16_t)v.stall; w[MT_TW_QUICKACK] = (uint16_t)v.quickack;
    T_EQ(tune_call(&x, w, MT_TUNE_ABI_VERSION, MT_TUNE_OP_SET), 0);
    mt_tune_get(x.d->s, &t);
    T_EQ(t.rto_min, MT_TCP_RTO_MIN); T_EQ(t.rto_init, MT_TCP_RTO_INIT); T_EQ(t.delack_ms, MT_TCP_DELACK_MS);
    T_EQ(t.stall_ms, MT_TCP_STALL_MS); T_EQ(t.quickack, MT_TCP_QUICKACK);
}

T_MAIN(test_pb_control_get_set, test_presets_apply_and_default_restores, test_defaults_equal_defines, test_set_get_round_trip, test_each_bound_refused,
       test_delack0_acks_first_segment_at_once, test_delack_time_is_used, test_quickack_255_never_arms_delack,
       test_quickack_n_counts, test_rto_500_fires_at_500, test_rto_min_floor_after_sample, test_stall_ms_used,
       test_running_timer_keeps_deadline)
#endif
