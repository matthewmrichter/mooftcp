#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "pair.h"
#include <stdio.h>
#include "mt_int.h"
#include "net.h"

#define QCAP 4096

typedef struct { uint32_t due, seq; uint16_t len; uint8_t to; uint8_t f[1514]; } qent;

typedef struct { struct pair *p; int who; } txctx;

struct pair {
    pair_faults f;
    uint32_t rng, now, seq, stuck, nsent, ndrop, ncorrupt;
    mt_stack *s[2];
    void *arena[2], *raw[2];
    txctx ctx[2];
    qent *q;
    int n;
};

static uint32_t rnd(pair *p) {
    uint32_t x = p->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return p->rng = x;
}
static int chance(pair *p, uint32_t ppm) { return ppm && rnd(p) % 1000000u < ppm; }

static void enqueue(pair *p, int to, const uint8_t *fr, uint16_t len, uint32_t extra) {
    qent *e;
    if (p->n >= QCAP) { p->ndrop++; return; }
    e = &p->q[p->n++];
    e->due = p->now + p->f.delay_ms + extra;
    e->seq = p->seq++;
    e->len = len;
    e->to = (uint8_t)to;
    memcpy(e->f, fr, len);
}

static int pair_tx(void *vctx, const uint8_t *frame, uint16_t len) {
    txctx *c = (txctx *)vctx;
    pair *p = c->p;
    int to = !c->who;
    uint8_t tmp[1514];
    uint32_t extra = 0;
    int corrupt = 0;
    if (len > 1514) return 1;
    p->nsent++;
    if (getenv("PAIR_TRACE")) {          /* debug aid: one line per transmitted frame */
        tcpv v;
        if (view_tcp(frame, len, &v))
            fprintf(stderr, "%8u %c->%c seq %u ack %u fl %02x len %u win %u\n", p->now,
                    c->who ? 'B' : 'A', c->who ? 'A' : 'B', v.seq, v.ack, v.flags, v.dlen, v.win);
        else
            fprintf(stderr, "%8u %c->%c non-tcp len %u\n", p->now, c->who ? 'B' : 'A',
                    c->who ? 'A' : 'B', len);
    }
    if (chance(p, p->f.drop_ppm)) { p->ndrop++; return 0; }
    memcpy(tmp, frame, len);
    if (chance(p, p->f.corrupt_ppm)) {
        uint32_t at = rnd(p) % len;
        tmp[at] ^= (uint8_t)(1u + rnd(p) % 255u);
        if (at < 14 || (tmp[12] == 0x08 && tmp[13] == 0x06)) { p->ndrop++; return 0; }  /* FCS */
        corrupt = 1;
        p->ncorrupt++;
    }
    (void)corrupt;
    if (chance(p, p->f.reorder_ppm)) extra = 1u + rnd(p) % 20u;
    enqueue(p, to, tmp, len, extra);
    if (chance(p, p->f.dup_ppm)) enqueue(p, to, tmp, len, rnd(p) % 4u);
    return 0;
}

/* C99 has no aligned_alloc: over-allocate and align by hand (raw pointer kept for free). */
static void *arena_new(uint32_t sz, void **raw) {
    uintptr_t u;
    *raw = calloc(1, (size_t)sz + 64);
    if (!*raw) return NULL;
    u = ((uintptr_t)*raw + 63u) & ~(uintptr_t)63u;
    return (void *)u;
}

pair *pair_new(const pair_faults *f, uint32_t start_ms) {
    static const uint8_t mac[2][6] = {{0x02, 0, 0, 0, 0xA0, 0xC8}, {0x02, 0, 0, 0, 0xB0, 0x0A}};
    static const mt_ip4 ip[2] = {PAIR_A_IP, PAIR_B_IP};
    pair *p = (pair *)calloc(1, sizeof *p);
    mt_limits lim;
    int i;
    if (!p) return NULL;
    p->f = *f;
    p->rng = f->seed ? f->seed : 1u;
    p->now = start_ms;
    p->q = (qent *)calloc(QCAP, sizeof(qent));
    if (!p->q) { free(p); return NULL; }
    mt_limits_default(&lim);
    for (i = 0; i < 2; i++) {
        mt_link link;
        mt_ifconfig c;
        p->ctx[i].p = p;
        p->ctx[i].who = i;
        link.ctx = &p->ctx[i];
        link.tx = pair_tx;
        p->arena[i] = arena_new(mt_stack_size(&lim), &p->raw[i]);
        p->s[i] = mt_stack_init(p->arena[i], mt_stack_size(&lim), &lim, &link, 1000u + (uint32_t)i);
        if (!p->s[i]) { pair_free(p); return NULL; }
        memset(&c, 0, sizeof c);
        c.addr = ip[i];
        c.mask = 0xFFFFFF00u;
        c.mtu = 1500;
        memcpy(c.mac, mac[i], 6);
        mt_set_config(p->s[i], &c);
    }
    return p;
}

void pair_free(pair *p) {
    if (!p) return;
    free(p->raw[0]); free(p->raw[1]); free(p->q); free(p);
}

mt_stack *pair_a(pair *p) { return p->s[0]; }
mt_stack *pair_b(pair *p) { return p->s[1]; }
uint32_t pair_now(const pair *p) { return p->now; }
uint32_t pair_stuck(const pair *p) { return p->stuck; }
uint32_t pair_sent(const pair *p) { return p->nsent; }
uint32_t pair_dropped(const pair *p) { return p->ndrop; }
uint32_t pair_corrupted(const pair *p) { return p->ncorrupt; }

/* Deliver the oldest frame due now. Returns 1 if one was delivered. */
static int deliver_one(pair *p) {
    int i, best = -1;
    qent e;
    for (i = 0; i < p->n; i++)
        if (p->q[i].due == p->now && (best < 0 || p->q[i].seq < p->q[best].seq)) best = i;
    if (best < 0) return 0;
    e = p->q[best];
    p->q[best] = p->q[--p->n];
    mt_input(p->s[e.to], e.f, e.len, p->now);
    return 1;
}

void pair_run(pair *p, uint32_t ms) {
    uint32_t target = p->now + ms;
    for (;;) {
        int progress = 0, i;
        uint32_t next = MT_NO_DEADLINE, d;
        while (deliver_one(p)) progress = 1;
        for (i = 0; i < 2; i++) {
            if (mt_next_deadline(p->s[i], p->now) == 0) {
                mt_tick(p->s[i], p->now);
                progress = 1;
                if (mt_next_deadline(p->s[i], p->now) == 0) {
                    p->stuck++;
                    progress = 0;       /* do not spin on a stack that cannot make progress */
                }
            }
        }
        if (progress) continue;
        for (i = 0; i < p->n; i++) {
            d = p->q[i].due - p->now;
            if (d < next) next = d;
        }
        for (i = 0; i < 2; i++) {
            d = mt_next_deadline(p->s[i], p->now);
            if (d < next) next = d;
        }
        if (next == MT_NO_DEADLINE || next == 0 || next > target - p->now) {
            p->now = target;    /* next == 0 only for a stuck stack: give up on this step */
            return;
        }
        p->now += next;
    }
}
