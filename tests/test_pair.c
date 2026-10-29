#include "t.h"
#include "pair.h"
#include "mt_util.h"
#include "net.h"
#include "mt_int.h"

/* Two real stacks over a faulty link (see pair.h). A = .200 connects, B = .10 listens on 7000.
 * Streams are pseudo-random patterns checked byte for byte. A transfer that does not finish
 * within BUDGET_MS of virtual time fails (rather than looping forever). */

#define BUDGET_MS 600000u
#define STEP_MS 5u
#define PORT 7000

typedef struct {
    pair *p;
    mt_tcp *t;
    uint8_t *buf;                   /* connection buffer handed to the core */
    uint8_t *sbuf; uint32_t stotal, sent;
    uint8_t *rbuf; uint32_t rcap, got, overflow;
    uint8_t rsalt;
    int close_after, close_on_fin, closed;
    int estab, peer_fin, term, reason;
    uint32_t est_t;                 /* virtual time of ESTABLISHED */
    int drain;                      /* read in the DATA callback */
    uint32_t slow_bytes, slow_ms, last_read;
} ep;

static uint8_t pat(uint32_t i, uint8_t salt) { return (uint8_t)(((i * 2654435761u) >> 24) ^ salt); }

static void ep_pump(ep *e) {
    while (e->t && e->sent < e->stotal && e->estab) {
        uint32_t n = e->stotal - e->sent;
        int32_t r;
        if (n > 4096) n = 4096;
        r = mt_tcp_send(e->t, e->sbuf + e->sent, n, e->sent + n == e->stotal, 0, pair_now(e->p));
        if (r <= 0) break;
        e->sent += (uint32_t)r;
    }
    if (e->t && e->close_after && !e->closed && e->sent == e->stotal && e->estab) {
        e->closed = 1;
        T_EQ(mt_tcp_close(e->t, pair_now(e->p)), MT_OK);
    }
}

static void ep_drain(ep *e, uint32_t max) {
    uint8_t tmp[8192];
    for (;;) {
        uint32_t want = max && max < sizeof tmp ? max : (uint32_t)sizeof tmp;
        uint32_t n = mt_tcp_recv(e->t, tmp, want, pair_now(e->p));
        if (!n) break;
        if (e->got + n <= e->rcap) memcpy(e->rbuf + e->got, tmp, n); else e->overflow = 1;
        e->got += n;
        if (max) break;
    }
}

static void ep_cb(mt_tcp *t, mt_tcp_event ev, uint32_t arg, void *user) {
    ep *e = (ep *)user;
    switch (ev) {
    case MT_TCP_EV_ESTABLISHED: e->estab = 1; e->est_t = pair_now(e->p); ep_pump(e); break;
    case MT_TCP_EV_DATA: if (e->drain) ep_drain(e, 0); break;
    case MT_TCP_EV_CLOSING:
        e->peer_fin = 1;
        if (e->drain) ep_drain(e, 0);
        if (e->close_on_fin && !e->closed) { e->closed = 1; T_EQ(mt_tcp_close(t, pair_now(e->p)), MT_OK); }
        break;
    case MT_TCP_EV_SEND_SPACE: ep_pump(e); break;
    case MT_TCP_EV_TERMINATE: e->term = 1; e->reason = (int)arg; break;
    default: break;
    }
}

static void ep_init(ep *e, pair *p, mt_stack *s, uint32_t stotal, uint8_t ssalt, uint32_t rtotal,
                    uint8_t rsalt) {
    uint32_t i, bl = 32768;
    memset(e, 0, sizeof *e);
    e->p = p;
    e->buf = (uint8_t *)malloc(bl + 1) + 1;           /* odd address on purpose */
    e->stotal = stotal; e->rcap = rtotal + 64; e->rsalt = rsalt;
    e->sbuf = (uint8_t *)malloc(stotal + 1);
    e->rbuf = (uint8_t *)malloc(e->rcap);
    for (i = 0; i < stotal; i++) e->sbuf[i] = pat(i, ssalt);
    e->drain = 1;
    e->t = mt_tcp_create(s, e->buf, bl, ep_cb, e);
    T_CHECK(e->t != NULL);
}

static void ep_free(ep *e) {
    if (e->t) mt_tcp_release(e->t);
    free(e->buf - 1); free(e->sbuf); free(e->rbuf);
}

static int ep_rx_ok(const ep *e, uint32_t total) {
    uint32_t i;
    if (e->got != total || e->overflow) return 0;
    for (i = 0; i < total; i++) if (e->rbuf[i] != pat(i, e->rsalt)) return 0;
    return 1;
}

typedef struct {
    pair_faults f; uint32_t start;
    uint32_t a_send, b_send;
    int a_close_after, b_close_after, a_close_on_fin, b_close_on_fin;
    uint32_t a_slow_bytes, a_slow_ms;
    int fault;                     /* directed fault (TL_*), 0 = none */
} scen;

typedef struct { ep a, b; pair *p; int done; uint32_t t0, elapsed; } run;
static int tl_filter(void *user, int from, const uint8_t *f, uint16_t len);
static void tl_reset(int kind, uint32_t total);

static int data_done(run *r, const scen *s) {
    return r->a.got == s->b_send && r->b.got == s->a_send;
}
static int fully_done(run *r, const scen *s) {
    if (!data_done(r, s)) return 0;
    if ((s->a_close_after || s->a_close_on_fin) && !r->a.term) return 0;
    if ((s->b_close_after || s->b_close_on_fin) && !r->b.term) return 0;
    return 1;
}

/* Build the pair and start the connection. Caller frees with run_free. */
static void run_start(run *r, const scen *s) {
    memset(r, 0, sizeof *r);
    r->p = pair_new(&s->f, s->start);
    T_CHECK(r->p != NULL);
    ep_init(&r->a, r->p, pair_a(r->p), s->a_send, 0x5A, s->b_send, 0xC3);
    ep_init(&r->b, r->p, pair_b(r->p), s->b_send, 0xC3, s->a_send, 0x5A);
    r->a.close_after = s->a_close_after; r->a.close_on_fin = s->a_close_on_fin;
    r->b.close_after = s->b_close_after; r->b.close_on_fin = s->b_close_on_fin;
    if (s->a_slow_bytes) {
        r->a.drain = 0; r->a.slow_bytes = s->a_slow_bytes; r->a.slow_ms = s->a_slow_ms;
        r->a.last_read = pair_now(r->p);
    }
    if (s->fault) { tl_reset(s->fault, s->b_send); pair_set_filter(r->p, tl_filter, r->p); }
    T_EQ(mt_tcp_listen(r->b.t, 0, 0, PORT), MT_OK);
    r->t0 = pair_now(r->p);
    T_EQ(mt_tcp_open(r->a.t, PAIR_B_IP, PORT, 0, pair_now(r->p)), MT_OK);
}

static void run_until_done(run *r, const scen *s) {
    while (!fully_done(r, s) && pair_now(r->p) - r->t0 < BUDGET_MS) {
        pair_run(r->p, STEP_MS);
        if (r->a.slow_bytes && pair_now(r->p) - r->a.last_read >= r->a.slow_ms) {
            r->a.last_read = pair_now(r->p);
            ep_drain(&r->a, r->a.slow_bytes);
        }
    }
    r->elapsed = pair_now(r->p) - r->t0;
    r->done = fully_done(r, s);
}

static void run_free(run *r) {
    ep_free(&r->a); ep_free(&r->b); pair_free(r->p);
}

static void check_run(run *r, const scen *s, const char *what, uint32_t seed) {
    if (!r->done) {
        fprintf(stderr, "%s seed %u: STALLED after %u virtual ms: A got %u/%u B got %u/%u "
                "(A term %d B term %d)\n", what, (unsigned)seed, (unsigned)r->elapsed,
                (unsigned)r->a.got, (unsigned)s->b_send, (unsigned)r->b.got,
                (unsigned)s->a_send, r->a.term, r->b.term);
        t_failed = 1;
    }
    T_CHECK(ep_rx_ok(&r->a, s->b_send));
    T_CHECK(ep_rx_ok(&r->b, s->a_send));
    T_EQ(pair_stuck(r->p), 0);
    if (t_failed) fprintf(stderr, "  (%s seed %u)\n", what, (unsigned)seed);
}

static void lossy(pair_faults *f, uint32_t seed) {
    memset(f, 0, sizeof *f);
    f->drop_ppm = 20000; f->dup_ppm = 10000; f->reorder_ppm = 20000;
    f->corrupt_ppm = 5000; f->delay_ms = 5; f->seed = seed;
    if (getenv("PAIR_MULT")) {          /* soak knob: scale fault rates, e.g. PAIR_MULT=5 */
        uint32_t m = (uint32_t)atoi(getenv("PAIR_MULT"));
        const char *only = getenv("PAIR_ONLY");     /* e.g. PAIR_ONLY=reorder */
        if (!only || !strcmp(only, "drop")) f->drop_ppm *= m; else f->drop_ppm = 0;
        if (!only || !strcmp(only, "dup")) f->dup_ppm *= m; else f->dup_ppm = 0;
        if (!only || !strcmp(only, "reorder")) f->reorder_ppm *= m; else f->reorder_ppm = 0;
        if (!only || !strcmp(only, "corrupt")) f->corrupt_ppm *= m; else f->corrupt_ppm = 0;
    }
    if (getenv("PAIR_DELAY")) f->delay_ms = (uint32_t)atoi(getenv("PAIR_DELAY"));
}

/* Soak knob: PAIR_SEEDS overrides how many seeds the looping tests run. */
static uint32_t nseeds(uint32_t def) {
    return getenv("PAIR_SEEDS") ? (uint32_t)atoi(getenv("PAIR_SEEDS")) : def;
}

static void test_pair_bulk_clean(void) {
    scen s; run r;
    memset(&s, 0, sizeof s);
    s.f.delay_ms = 5; s.f.seed = 1;
    s.b_send = 256 * 1024;
    s.b_close_after = 1; s.a_close_on_fin = 1;
    run_start(&r, &s);
    run_until_done(&r, &s);
    check_run(&r, &s, "bulk_clean", 1);
    T_EQ(r.a.reason, MT_TERM_ULP_CLOSE);
    T_EQ(r.b.reason, MT_TERM_ULP_CLOSE);
    run_free(&r);
}

static void test_pair_bulk_lossy(void) {
    uint32_t seed, n_seeds = nseeds(20);
    uint32_t ndrop = 0, ncorrupt = 0;
    for (seed = 1; seed <= n_seeds; seed++) {
        scen s; run r;
        memset(&s, 0, sizeof s);
        lossy(&s.f, seed);
        s.b_send = 256 * 1024;
        s.b_close_after = 1; s.a_close_on_fin = 1;
        run_start(&r, &s);
        run_until_done(&r, &s);
        check_run(&r, &s, "bulk_lossy", seed);
        if (r.a.reason != MT_TERM_ULP_CLOSE || r.b.reason != MT_TERM_ULP_CLOSE)
            fprintf(stderr, "bulk_lossy seed %u: term reasons A %d B %d\n", (unsigned)seed,
                    r.a.reason, r.b.reason);
        T_EQ(r.a.reason, MT_TERM_ULP_CLOSE);
        T_EQ(r.b.reason, MT_TERM_ULP_CLOSE);
        if (!getenv("PAIR_MULT")) T_CHECK(pair_dropped(r.p) > 0);   /* the faults happened */
        ndrop += pair_dropped(r.p); ncorrupt += pair_corrupted(r.p);
        /* every corrupted frame was rejected by an IP or TCP checksum (none slipped through) */
        T_CHECK(mt_get_stats(pair_a(r.p))->ip_in_drops + mt_get_stats(pair_a(r.p))->tcp_in_drops +
                mt_get_stats(pair_b(r.p))->ip_in_drops + mt_get_stats(pair_b(r.p))->tcp_in_drops
                >= pair_corrupted(r.p));
        if (getenv("PAIR_VERBOSE"))
            fprintf(stderr, "seed %2u: %u ms virtual, %u frames, %u dropped\n", (unsigned)seed,
                    (unsigned)r.elapsed, (unsigned)pair_sent(r.p), (unsigned)pair_dropped(r.p));
        run_free(&r);
    }
    if (!getenv("PAIR_MULT")) { T_CHECK(ndrop > 100); T_CHECK(ncorrupt > 20); }
}

/* ---- interactive echo ---- */
typedef struct { pair *p; mt_tcp *t; int echo; uint32_t got, arrive; uint8_t last; } ie;

static void ie_cb(mt_tcp *t, mt_tcp_event ev, uint32_t arg, void *user) {
    ie *e = (ie *)user;
    (void)arg;
    if (ev == MT_TCP_EV_DATA) {
        uint8_t b[16];
        uint32_t n = mt_tcp_recv(t, b, sizeof b, pair_now(e->p));
        e->got += n;
        e->arrive = pair_now(e->p);
        if (n) e->last = b[0];
        if (e->echo && n) T_EQ(mt_tcp_send(t, b, n, 1, 0, pair_now(e->p)), (long long)n);
    }
}

static void interactive(uint32_t delay) {
    pair_faults f; pair *p; ie a, b; static uint8_t ba[8192], bb[8192];
    mt_tcp_info ia;
    int i;
    memset(&f, 0, sizeof f);
    f.delay_ms = delay; f.seed = 7;
    p = pair_new(&f, 100);
    T_CHECK(p != NULL);
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.p = b.p = p; b.echo = 1;
    a.t = mt_tcp_create(pair_a(p), ba, sizeof ba, ie_cb, &a);
    b.t = mt_tcp_create(pair_b(p), bb, sizeof bb, ie_cb, &b);
    T_EQ(mt_tcp_listen(b.t, 0, 0, PORT), MT_OK);
    T_EQ(mt_tcp_open(a.t, PAIR_B_IP, PORT, 0, pair_now(p)), MT_OK);
    pair_run(p, 100 + 8 * delay);                        /* ARP + handshake */
    mt_tcp_get_info(a.t, &ia);
    T_EQ(ia.state, MT_TCP_ESTABLISHED);
    for (i = 0; i < 200; i++) {
        uint8_t byte = (uint8_t)(i * 13 + 1);
        uint32_t t0 = pair_now(p), g0 = a.got;
        T_EQ(mt_tcp_send(a.t, &byte, 1, 1, 0, t0), 1);
        if (delay == 0) {
            pair_run(p, 0);
            T_EQ(a.got, g0 + 1);                         /* echoed within this very step */
            T_EQ(pair_now(p), t0);
        } else {
            pair_run(p, 2 * delay - 1);
            T_EQ(a.got, g0);                             /* not a millisecond early */
            pair_run(p, 1);
            T_EQ(a.got, g0 + 1);
        }
        T_EQ(a.arrive - t0, 2 * delay);                  /* RTT == 2 * link delay exactly */
        T_EQ(a.last, byte);
        if (t_failed) break;
    }
    T_EQ(pair_stuck(p), 0);
    mt_tcp_release(a.t); mt_tcp_release(b.t);
    pair_free(p);
}

static void test_pair_interactive_clean(void) {
    interactive(0);
    interactive(1);
    interactive(5);
    interactive(40);
}

static void test_pair_bidirectional_close(void) {
    uint32_t seed;
    for (seed = 0; seed <= nseeds(5); seed++) {
        scen s; run r;
        memset(&s, 0, sizeof s);
        if (seed) lossy(&s.f, seed); else s.f.delay_ms = 5;
        s.a_send = s.b_send = 64 * 1024;
        s.a_close_after = s.b_close_after = 1;
        run_start(&r, &s);
        run_until_done(&r, &s);
        check_run(&r, &s, "bidirectional_close", seed);
        T_EQ(r.a.reason, MT_TERM_ULP_CLOSE);
        T_EQ(r.b.reason, MT_TERM_ULP_CLOSE);
        run_free(&r);
    }
}

static void test_pair_wrap(void) {
    uint32_t seed;
    for (seed = 1; seed <= nseeds(5); seed++) {
        scen s; run r;
        memset(&s, 0, sizeof s);
        lossy(&s.f, seed);
        s.start = 0xFFFFF000u;
        s.b_send = 64 * 1024;
        s.b_close_after = 1; s.a_close_on_fin = 1;
        s.a_slow_bytes = 512; s.a_slow_ms = 100;         /* ~13 s: the clock wraps mid-transfer */
        run_start(&r, &s);
        run_until_done(&r, &s);
        check_run(&r, &s, "wrap", seed);
        T_CHECK(pair_now(r.p) < 0x80000000u);            /* the clock really wrapped */
        T_EQ(r.a.reason, MT_TERM_ULP_CLOSE);
        T_EQ(r.b.reason, MT_TERM_ULP_CLOSE);
        run_free(&r);
    }
}

static void test_pair_slow_reader(void) {
    scen s; run r;
    memset(&s, 0, sizeof s);
    s.f.delay_ms = 5; s.f.seed = 3;
    s.b_send = 64 * 1024;
    s.a_slow_bytes = 512; s.a_slow_ms = 200;             /* neither side closes */
    run_start(&r, &s);
    run_until_done(&r, &s);
    check_run(&r, &s, "slow_reader", 3);
    T_EQ(r.a.term, 0);
    T_EQ(r.b.term, 0);
    /* an idle, healthy connection must survive well past the 120 s ULP timeout */
    pair_run(r.p, 200000);
    T_EQ(r.a.term, 0);
    T_EQ(r.b.term, 0);
    T_EQ(pair_stuck(r.p), 0);
    run_free(&r);
}

/* ---- directed single-frame losses on an otherwise clean link ---- */
enum { TL_ARP = 1, TL_SYN, TL_SYNACK, TL_AFIN, TL_FINAL_ACK, TL_TAIL, TL_MID, TL_FINAL_ACK3 };
static struct { int kind, hits, saw_a_fin, saw_b_fin; uint32_t b_isn, total, drop_seq, drop_t, resend_dt; } tl;

static void tl_reset(int kind, uint32_t total) {
    memset(&tl, 0, sizeof tl);
    tl.kind = kind; tl.total = total;
}

static int tl_filter(void *user, int from, const uint8_t *f, uint16_t len) {
    tcpv v; arpv a;
    pair *p = (pair *)user;
    if (tl.kind == TL_ARP) {
        if (from == 0 && !tl.hits && view_arp(f, len, &a) && a.op == 1) { tl.hits++; return 1; }
        return 0;
    }
    if (!view_tcp(f, len, &v)) return 0;
    if (from == 1 && (v.flags & TF_SYN)) tl.b_isn = v.seq;
    if (tl.drop_t && !tl.resend_dt && from == 1 && v.dlen && v.seq == tl.drop_seq)
        tl.resend_dt = pair_now(p) - (tl.drop_t - 1u);  /* first resend of the lost segment */
    if (v.flags & TF_FIN) { if (from) tl.saw_b_fin = 1; else tl.saw_a_fin = 1; }
    switch (tl.kind) {
    case TL_SYN:    if (from == 0 && v.flags == TF_SYN && !tl.hits) { tl.hits++; return 1; } break;
    case TL_SYNACK: if (from == 1 && v.flags == (TF_SYN | TF_ACK) && !tl.hits) { tl.hits++; return 1; } break;
    case TL_AFIN:   if (from == 0 && (v.flags & TF_FIN) && !tl.hits) { tl.hits++; return 1; } break;
    case TL_FINAL_ACK: case TL_FINAL_ACK3:       /* B's ACK of A's FIN (B is in TIME_WAIT) */
        if (from == 1 && tl.saw_a_fin && tl.saw_b_fin && v.flags == TF_ACK && !v.dlen &&
            tl.hits < (tl.kind == TL_FINAL_ACK ? 1 : 3)) { tl.hits++; return 1; }
        break;
    case TL_TAIL:
        if (from == 1 && v.dlen && v.seq + v.dlen == tl.b_isn + 1 + tl.total && !tl.hits) {
            tl.hits++; return 1; }
        break;
    case TL_MID:
        if (from == 1 && v.dlen && v.seq - (tl.b_isn + 1) >= 65536u && !tl.hits) {
            tl.hits++; tl.drop_seq = v.seq; tl.drop_t = pair_now(p) + 1u; return 1; }
        break;
    }
    return 0;
}

static void targeted(int kind, uint32_t delay, uint32_t total, const char *what, uint32_t *elapsed,
                     uint32_t *rexmt, uint32_t *resend_dt, uint32_t *est) {
    scen s; run r;
    memset(&s, 0, sizeof s);
    s.f.delay_ms = delay; s.f.seed = 1;
    s.b_send = total; s.b_close_after = 1; s.a_close_on_fin = 1;
    s.fault = kind;
    run_start(&r, &s);
    run_until_done(&r, &s);
    check_run(&r, &s, what, (uint32_t)kind);
    T_EQ(r.a.reason, MT_TERM_ULP_CLOSE);
    T_EQ(r.b.reason, MT_TERM_ULP_CLOSE);
    if (kind == TL_FINAL_ACK3) T_EQ(tl.hits, 3);
    else if (kind) T_EQ(tl.hits, 1);                 /* kind 0: no directed fault (tl not reset) */
    if (elapsed) *elapsed = r.elapsed;
    if (est) *est = r.a.est_t - r.t0;
    if (resend_dt) *resend_dt = tl.resend_dt;
    if (rexmt) *rexmt = mt_get_stats(pair_a(r.p))->tcp_rexmt + mt_get_stats(pair_b(r.p))->tcp_rexmt;
    run_free(&r);
}

/* Repair cost of one mid-stream loss on a clean link. NewReno resends one hole per partial ACK,
 * so the penalty over the lossless transfer is the detection time (3 duplicate ACKs, about one
 * RTT after the loss) plus one RTT per resent segment (`rex` of them): (rex + 1) RTTs. A receiver
 * that held each lone resent segment for the 100 ms delayed ACK would add 100 ms per hole, so
 * the bound is (rex + 2) RTTs (one RTT of slack) + 2 ms of timer rounding; the delayed ACKs
 * alone would exceed it by far. */
static void mid_loss_penalty(uint32_t delay) {
    uint32_t clean, loss, rex, rex0, dt, rtt = 2 * delay;
    targeted(0, delay, 131072, "penalty clean", &clean, &rex0, NULL, NULL);
    targeted(TL_MID, delay, 131072, "penalty loss", &loss, &rex, &dt, NULL);
    T_EQ(rex0, 0);
    T_CHECK(rex >= 1 && rex <= 8);
    T_CHECK(loss > clean);
    T_CHECK(loss - clean <= (rex + 2) * rtt + 2);
    if (t_failed)
        fprintf(stderr, "  delay %u: clean %u ms, loss %u ms, %u resends\n", (unsigned)delay,
                (unsigned)clean, (unsigned)loss, (unsigned)rex);
}

static void test_pair_loss_penalty(void) {
    mid_loss_penalty(1);
    mid_loss_penalty(5);
}

static void test_pair_targeted_loss(void) {
    uint32_t clean, loss, rex, rex0, dt, est;
    targeted(TL_ARP, 5, 32768, "arp request lost", NULL, NULL, NULL, &est);
    T_CHECK(est >= 1000 && est < 1500);              /* the 2nd ARP request (+1 s) got through */
    targeted(TL_SYN, 5, 32768, "syn lost", NULL, NULL, NULL, NULL);
    targeted(TL_SYNACK, 5, 32768, "syn-ack lost", NULL, NULL, NULL, NULL);
    targeted(TL_AFIN, 5, 32768, "A's fin lost", NULL, NULL, NULL, NULL);
    targeted(TL_FINAL_ACK, 5, 32768, "final ack lost", NULL, NULL, NULL, NULL);   /* TIME_WAIT must re-ACK */
    targeted(TL_TAIL, 5, 32768, "tail segment lost", NULL, NULL, NULL, NULL);     /* RTO-only recovery */
    /* One mid-stream loss in a 128 KB transfer must be repaired by fast retransmit: the lost
     * segment is resent well before the RTO (>= RTO_MIN) could fire. The receiver keeps no
     * out-of-order data, so the (up to ~5) segments already in flight behind the hole are resent
     * too: bounded, not a storm. */
    targeted(0, 5, 131072, "mid clean", &clean, &rex0, NULL, NULL);
    targeted(TL_MID, 5, 131072, "mid-stream loss", &loss, &rex, &dt, NULL);
    T_EQ(rex0, 0);
    T_CHECK(rex >= 1 && rex <= 8);
    T_CHECK(dt > 0 && dt < MT_TCP_RTO_MIN);
    /* Slow link (400 ms each way), the first three final ACKs lost. A's FIN retransmits at +1 s,
     * +2 s, +4 s and each arrival renews B's 2 s TIME_WAIT, so the fourth FIN finds B gone and is
     * answered with an RST; A (LAST_ACK, all data acked) must still report a clean close. (With
     * only two lost ACKs the third FIN arrives exactly as TIME_WAIT expires: a tie.) */
    targeted(TL_FINAL_ACK3, 400, 8192, "three final acks lost, slow link", NULL, NULL, NULL, NULL);
}

T_MAIN(test_pair_bulk_clean, test_pair_bulk_lossy, test_pair_interactive_clean,
       test_pair_bidirectional_close, test_pair_wrap, test_pair_slow_reader, test_pair_targeted_loss,
       test_pair_loss_penalty)
