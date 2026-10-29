#include "t.h"
#include "net.h"
#include "mt_int.h"
#include "mt_util.h"

typedef struct { int data, icmp; uint32_t arg; mt_udp *u; } cbs;
static void cb(mt_udp *u, mt_udp_event ev, uint32_t arg, void *user) {
    cbs *c = (cbs *)user;
    c->u = u;
    if (ev == MT_UDP_EV_DATA) c->data++;
    else { c->icmp++; c->arg = arg; }
}

static mt_stack *setup(fake_link *fl) {
    mt_stack *s = tstack_new(fl, T_ME);
    mt_arp_insert(s, T_PEER, T_PEER_MAC, 0);
    return s;
}

/* UDP datagram (cksum: 0 = none, 1 = correct, 2 = wrong) wrapped in an IP frame at buf+odd. */
static uint16_t mk_udp_ip(uint8_t *f, mt_ip4 src, mt_ip4 dst, uint16_t sp, uint16_t dp,
                          const uint8_t *d, uint16_t n, int cksum, int ulen_adj) {
    uint8_t pl[1500];
    uint32_t t;
    mt_wr16(pl, sp); mt_wr16(pl + 2, dp);
    mt_wr16(pl + 4, (uint16_t)(8 + n + ulen_adj));
    mt_wr16(pl + 6, 0);
    if (n) memcpy(pl + 8, d, n);
    if (cksum) {
        t = (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + 17u + (uint32_t)(8 + n);
        t = mt_cksum_add(t, pl, (uint32_t)8 + n);
        t = mt_cksum_fold(t);
        if (t == 0) t = 0xFFFF;
        if (cksum == 2) t ^= 0x1234;
        mt_wr16(pl + 6, (uint16_t)t);
    }
    return mk_ip(f, src, dst, 17, 9, 0, pl, (uint16_t)(8 + n));
}

static void inject(mt_stack *s, mt_ip4 src, mt_ip4 dst, uint16_t sp, uint16_t dp,
                   const uint8_t *d, uint16_t n, int cksum, int adj, int odd) {
    static uint8_t buf[1600];
    uint8_t *f = buf + (odd & 1);
    uint16_t l = mk_udp_ip(f, src, dst, sp, dp, d, n, cksum, adj);
    mt_input(s, f, l, 10);
}

static int verify_cksum(mt_ip4 src, mt_ip4 dst, const uint8_t *u, uint16_t len) {
    uint32_t t = (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + 17u + len;
    return mt_cksum_fold(mt_cksum_add(t, u, len)) == 0;
}

static void test_udp_roundtrip(void) {
    fake_link fl; mt_stack *s = setup(&fl);
    static uint8_t rb[2049];
    cbs c = {0, 0, 0, 0};
    mt_udp *u = mt_udp_create(s, 5000, rb + 1, 2048, cb, &c);   /* odd app buffer */
    mt_udp_dgram g;
    T_CHECK(u != NULL);
    T_EQ(mt_udp_port(u), 5000);
    inject(s, T_PEER, T_ME, 53, 5000, (const uint8_t *)"abc", 3, 1, 0, 1);
    T_EQ(c.data, 1); T_CHECK(c.u == u);
    T_EQ(mt_udp_queued(u), 1);
    T_CHECK(mt_udp_peek(u, &g));
    T_EQ(g.src, T_PEER); T_EQ(g.sport, 53); T_EQ(g.len, 3);
    T_MEMEQ(g.data, "abc", 3);
    mt_udp_done(u);
    T_EQ(mt_udp_peek(u, &g), 0);
    T_EQ(mt_udp_queued(u), 0);
    T_EQ(mt_get_stats(s)->udp_in, 1);
    /* checksum 0 = unchecked; wrong checksum dropped; empty datagram ok */
    inject(s, T_PEER, T_ME, 7, 5000, (const uint8_t *)"x", 1, 0, 0, 0);
    T_EQ(mt_udp_queued(u), 1);
    inject(s, T_PEER, T_ME, 7, 5000, (const uint8_t *)"yy", 2, 2, 0, 0);
    T_EQ(mt_udp_queued(u), 1); T_EQ(mt_get_stats(s)->udp_drops, 1);
    inject(s, T_PEER, T_ME, 8, 5000, NULL, 0, 1, 0, 0);
    T_EQ(mt_udp_queued(u), 2);
    T_CHECK(mt_udp_peek(u, &g)); T_EQ(g.len, 1); mt_udp_done(u);
    T_CHECK(mt_udp_peek(u, &g)); T_EQ(g.len, 0); T_EQ(g.sport, 8); mt_udp_done(u);
    /* after release: no callback, port is closed */
    mt_udp_release(u);
    c.data = 0; fl.n = 0;
    inject(s, T_PEER, T_ME, 53, 5000, (const uint8_t *)"abc", 3, 1, 0, 0);
    T_EQ(c.data, 0);
    T_EQ(fl.n, 1);
    T_CHECK(mt_udp_create(s, 5000, rb, 2048, cb, &c) != NULL);   /* port reusable */
}

static void test_udp_send_fields(void) {
    fake_link fl; mt_stack *s = setup(&fl);
    static uint8_t rb[1024];
    cbs c = {0, 0, 0, 0};
    mt_udp *u = mt_udp_create(s, 5000, rb, sizeof rb, cb, &c);
    uint8_t d[10]; ipv v; int i;
    for (i = 0; i < 10; i++) d[i] = (uint8_t)(i + 1);
    T_EQ(mt_udp_send(u, T_PEER, 7, d, 10, 1, 20), MT_OK);
    T_EQ(fl.n, 1);
    T_CHECK(view_ip(fl.f[0], fl.len[0], &v));
    T_EQ(v.proto, 17); T_EQ(v.len, 18);
    T_EQ(mt_rd16(v.payload), 5000); T_EQ(mt_rd16(v.payload + 2), 7);
    T_EQ(mt_rd16(v.payload + 4), 18);
    T_CHECK(mt_rd16(v.payload + 6) != 0);
    T_CHECK(verify_cksum(T_ME, T_PEER, v.payload, v.len));
    T_MEMEQ(v.payload + 8, d, 10);
    fake_clear(&fl);
    T_EQ(mt_udp_send(u, T_PEER, 7, d, 9, 0, 20), MT_OK);   /* odd length, no checksum */
    T_CHECK(view_ip(fl.f[0], fl.len[0], &v));
    T_EQ(v.len, 17); T_EQ(mt_rd16(v.payload + 6), 0);
    T_MEMEQ(v.payload + 8, d, 9);
    fake_clear(&fl);
    T_EQ(mt_udp_send(u, T_PEER, 7, d, 9, 1, 20), MT_OK);   /* odd length with checksum */
    T_CHECK(view_ip(fl.f[0], fl.len[0], &v));
    T_CHECK(verify_cksum(T_ME, T_PEER, v.payload, v.len));
    T_EQ(mt_get_stats(s)->udp_out, 3);
    T_EQ(mt_udp_send(u, T_PEER, 7, d, 0, 1, 20), MT_OK);   /* empty */
    T_EQ(mt_udp_send(u, T_PEER, 7, d, 65508u, 1, 20), MT_ERR_INVAL);
}

static void test_udp_unconfigured_send(void) {
    fake_link fl; mt_stack *s = tstack_new(&fl, 0);
    static uint8_t rb[1024];
    cbs c = {0, 0, 0, 0};
    mt_udp *u = mt_udp_create(s, 5000, rb, sizeof rb, cb, &c);
    T_CHECK(u != NULL);
    T_EQ(mt_udp_send(u, T_PEER, 7, rb, 4, 1, 0), MT_ERR_UNCONFIGURED);
    T_EQ(fl.n, 0);
}

static void test_udp_large_send_fragments(void) {
    fake_link fl; mt_stack *s = setup(&fl);
    static uint8_t rb[1024], d[4000], re[4008];
    cbs c = {0, 0, 0, 0};
    mt_udp *u = mt_udp_create(s, 5000, rb, sizeof rb, cb, &c);
    uint16_t i, got = 0;
    ipv v;
    for (i = 0; i < 4000; i++) d[i] = (uint8_t)(i * 7 + 3);
    T_EQ(mt_udp_send(u, T_PEER, 9, d, 4000, 1, 20), MT_OK);
    T_EQ(fl.n, 3);
    for (i = 0; i < fl.n; i++) {
        T_CHECK(view_ip(fl.f[i], fl.len[i], &v));
        T_EQ(v.frag * 8, got);
        T_EQ(v.flags != 0, i < 2);
        memcpy(re + got, v.payload, v.len);
        got = (uint16_t)(got + v.len);
    }
    T_EQ(got, 4008);
    T_EQ(mt_rd16(re + 4), 4008);
    T_CHECK(verify_cksum(T_ME, T_PEER, re, 4008));
    T_MEMEQ(re + 8, d, 4000);
    /* largest legal size reassembles in 45 fragments */
    fake_clear(&fl);
    {
        static uint8_t big[65507];
        T_EQ(mt_udp_send(u, T_PEER, 9, big, 65507, 1, 20), MT_OK);
        T_EQ(fl.n, 45);
    }
}

static void test_udp_closed_port_unreach(void) {
    fake_link fl; mt_stack *s = setup(&fl);
    ipv v; uint8_t d[4] = {1, 2, 3, 4};
    inject(s, T_PEER, T_ME, 4444, 9, d, 4, 1, 0, 1);
    T_EQ(fl.n, 1);
    T_CHECK(view_ip(fl.f[0], fl.len[0], &v));
    T_EQ(v.proto, 1); T_EQ(v.dst, T_PEER);
    T_EQ(v.payload[0], 3); T_EQ(v.payload[1], 3);
    T_EQ(v.payload[8], 0x45); T_EQ(v.payload[8 + 9], 17);
    T_EQ(mt_rd16(v.payload + 8 + 20), 4444); T_EQ(mt_rd16(v.payload + 8 + 22), 9);
    T_EQ(v.len, 8 + 20 + 8);
    T_EQ(mt_get_stats(s)->udp_noport, 1);
    /* broadcast destination: counted, no error */
    fake_clear(&fl);
    inject(s, T_PEER, 0xFFFFFFFFu, 4444, 9, d, 4, 0, 0, 0);
    T_EQ(fl.n, 0);
    T_EQ(mt_get_stats(s)->udp_noport, 2);
    /* bad checksum to closed port: dropped silently */
    inject(s, T_PEER, T_ME, 4444, 9, d, 4, 2, 0, 0);
    T_EQ(fl.n, 0);
}

static void test_udp_ephemeral_unique(void) {
    fake_link fl; mt_stack *s = setup(&fl);
    static uint8_t rb[8][1024];
    cbs c = {0, 0, 0, 0};
    mt_udp *u[8], *x;
    int i, j;
    for (i = 0; i < 8; i++) {
        u[i] = mt_udp_create(s, 0, rb[i], 1024, cb, &c);
        T_CHECK(u[i] != NULL);
        if (!u[i]) return;
        T_CHECK(mt_udp_port(u[i]) >= 49152);
        for (j = 0; j < i; j++) T_CHECK(mt_udp_port(u[i]) != mt_udp_port(u[j]));
    }
    T_CHECK(mt_udp_create(s, 0, rb[0], 1024, cb, &c) == NULL);
    mt_udp_release(u[3]);
    x = mt_udp_create(s, mt_udp_port(u[4]), rb[3], 1024, cb, &c);   /* port in use */
    T_CHECK(x == NULL);
    T_CHECK(mt_udp_create(s, 6000, rb[3], 1023, cb, &c) == NULL);   /* buffer too small */
    T_CHECK(mt_udp_create(s, 6000, rb[3], 1024, cb, &c) != NULL);
}

static void test_udp_ephemeral_wrap(void) {
    fake_link fl; mt_stack *s = setup(&fl);
    static uint8_t rb[1024];
    cbs c = {0, 0, 0, 0};
    mt_udp *a, *b;
    s->udp_eph = 65535;
    a = mt_udp_create(s, 0, rb, 1024, cb, &c);
    b = mt_udp_create(s, 0, rb, 1024, cb, &c);
    T_EQ(mt_udp_port(a), 65535);
    T_EQ(mt_udp_port(b), 49152);
}

static void test_udp_buffer_full_drop(void) {
    fake_link fl; mt_stack *s = setup(&fl);
    static uint8_t rb[1024];
    cbs c = {0, 0, 0, 0};
    mt_udp *u = mt_udp_create(s, 5000, rb, sizeof rb, cb, &c);
    uint8_t d[100]; mt_udp_dgram g; int i, n;
    for (i = 0; i < 20; i++) {
        memset(d, i + 1, sizeof d);
        inject(s, T_PEER, T_ME, (uint16_t)(100 + i), 5000, d, 100, 1, 0, 0);
    }
    n = (int)mt_udp_queued(u);
    T_EQ(n, 1024 / 108);
    T_EQ(mt_get_stats(s)->udp_drops, 20 - n);
    T_EQ(c.data, n);
    for (i = 0; i < n; i++) {
        T_CHECK(mt_udp_peek(u, &g));
        T_EQ(g.sport, 100 + i); T_EQ(g.len, 100);
        T_EQ(g.data[0], i + 1); T_EQ(g.data[99], i + 1);
        mt_udp_done(u);
    }
    T_EQ(mt_udp_peek(u, &g), 0);
}

/* Records wrap around the end of the buffer and stay contiguous and intact. */
static void test_udp_ring_wrap(void) {
    fake_link fl; mt_stack *s = setup(&fl);
    static uint8_t rb[1025];
    cbs c = {0, 0, 0, 0};
    mt_udp *u = mt_udp_create(s, 5000, rb + 1, 1024, cb, &c);
    uint8_t d[101]; mt_udp_dgram g; int i, round, next = 0, seen = 0;
    for (round = 0; round < 40; round++) {
        for (i = 0; i < 3; i++) {
            memset(d, next + 1, sizeof d);
            inject(s, T_PEER, T_ME, (uint16_t)next, 5000, d, 101, 0, 0, round);
            next++;
        }
        while (mt_udp_peek(u, &g)) {          /* drain all but the newest */
            if (mt_udp_queued(u) == 1 && (round & 1)) break;
            T_EQ(g.sport, seen); T_EQ(g.len, 101);
            T_EQ(g.data[0], seen + 1); T_EQ(g.data[100], seen + 1);
            mt_udp_done(u);
            seen++;
        }
    }
    T_EQ(mt_get_stats(s)->udp_drops, 0);
}

static void test_udp_icmp_event(void) {
    fake_link fl; mt_stack *s = setup(&fl);
    static uint8_t rb[1024];
    cbs c = {0, 0, 0, 0};
    mt_udp *u = mt_udp_create(s, 5000, rb, sizeof rb, cb, &c);
    uint8_t orig[28], m[8 + 28], f[1514];
    uint16_t l;
    /* original datagram: T_ME:5000 -> T_PEER:53 */
    l = mk_udp_ip(f, T_ME, T_PEER, 5000, 53, NULL, 0, 0, 0);
    memcpy(orig, f + 14, 28);
    memset(m, 0, 8);
    m[0] = 3; m[1] = 3;
    memcpy(m + 8, orig, 28);
    mt_wr16(m + 2, mt_cksum_fold(mt_cksum_add(0, m, sizeof m)));
    l = mk_ip(f, T_PEER, T_ME, 1, 5, 0, m, sizeof m);
    mt_input(s, f, l, 10);
    T_EQ(c.icmp, 1); T_EQ(c.arg, 0x0303); T_CHECK(c.u == u);
    /* error for another port: no event */
    mt_wr16(orig + 20, 5001);
    memcpy(m + 8, orig, 28);
    mt_wr16(m + 2, 0);
    mt_wr16(m + 2, mt_cksum_fold(mt_cksum_add(0, m, sizeof m)));
    l = mk_ip(f, T_PEER, T_ME, 1, 5, 0, m, sizeof m);
    mt_input(s, f, l, 10);
    T_EQ(c.icmp, 1);
}

static void test_udp_length_mismatch_dropped(void) {
    fake_link fl; mt_stack *s = setup(&fl);
    static uint8_t rb[1024];
    cbs c = {0, 0, 0, 0};
    mt_udp *u = mt_udp_create(s, 5000, rb, sizeof rb, cb, &c);
    uint8_t d[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    inject(s, T_PEER, T_ME, 1, 5000, d, 8, 0, 4, 0);    /* UDP length > IP payload */
    T_EQ(mt_udp_queued(u), 0); T_EQ(mt_get_stats(s)->udp_drops, 1);
    inject(s, T_PEER, T_ME, 1, 5000, d, 8, 0, -9, 0);   /* UDP length < 8 */
    T_EQ(mt_udp_queued(u), 0); T_EQ(mt_get_stats(s)->udp_drops, 2);
    inject(s, T_PEER, T_ME, 1, 5000, d, 8, 0, -4, 1);   /* shorter than IP payload: trimmed */
    T_EQ(mt_udp_queued(u), 1);
    {
        mt_udp_dgram g;
        T_CHECK(mt_udp_peek(u, &g)); T_EQ(g.len, 4);
    }
    T_EQ(c.data, 1);
    T_EQ(fl.n, 0);
}

T_MAIN(test_udp_roundtrip, test_udp_send_fields, test_udp_unconfigured_send,
       test_udp_large_send_fragments, test_udp_closed_port_unreach, test_udp_ephemeral_unique,
       test_udp_ephemeral_wrap, test_udp_buffer_full_drop, test_udp_ring_wrap,
       test_udp_icmp_event, test_udp_length_mismatch_dropped)
