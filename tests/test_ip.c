#include "t.h"
#include "net.h"
#include "mt_int.h"

static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* ---- dispatch spy (MT_TEST_HOOKS) ---- */
typedef struct {
    int n;
    uint8_t proto; mt_ip4 src, dst; uint16_t len;
    uint32_t hash;                 /* FNV-1a of the last payload */
    uint8_t last[4096];
} spy_t;

static void spy_cb(void *ctx, uint8_t proto, mt_ip4 src, mt_ip4 dst, const uint8_t *pl, uint16_t len) {
    spy_t *y = (spy_t *)ctx;
    uint32_t h = 2166136261u; uint16_t i;
    y->n++; y->proto = proto; y->src = src; y->dst = dst; y->len = len;
    for (i = 0; i < len; i++) h = (h ^ pl[i]) * 16777619u;
    y->hash = h;
    if (len <= sizeof y->last) memcpy(y->last, pl, len);
}

static mt_stack *new_stack(fake_link *fl, spy_t *y) {
    mt_stack *s = tstack_new(fl, T_ME);
    memset(y, 0, sizeof *y);
    s->ip_spy = spy_cb;
    s->ip_spy_ctx = y;
    return s;
}

/* Valid frames injected by these tests, replayed by test_ip_odd_alignment. */
static uint8_t rec_f[48][1520];
static uint16_t rec_len[48];
static uint32_t rec_now[48];
static int rec_n;

static void inject(mt_stack *s, const uint8_t *f, uint16_t n, uint32_t now) {
    if (rec_n < 48) { memcpy(rec_f[rec_n], f, n); rec_len[rec_n] = n; rec_now[rec_n++] = now; }
    mt_input(s, f, n, now);
}

static void fill(uint8_t *p, uint16_t n) {
    uint16_t i;
    for (i = 0; i < n; i++) p[i] = (uint8_t)(i * 7 + (i >> 8));
}

/* Send fragments (numbered 1..3 of a 3000-byte datagram) in the order given. */
static void send_frags(mt_stack *s, const uint8_t *pl, uint16_t id, const int *order, int n,
                       uint32_t now) {
    static uint8_t f[1520];
    int i;
    for (i = 0; i < n; i++) {
        uint16_t off = (uint16_t)((order[i] - 1) * 1480);
        uint16_t len = order[i] == 3 ? 40 : 1480;
        uint16_t ff = (uint16_t)(off / 8 | (order[i] == 3 ? 0 : 0x2000));
        uint16_t flen = mk_ip(f, T_PEER, T_ME, 253, id, ff, pl + off, len);
        inject(s, f, flen, now);
    }
}

static void test_ip_route_onlink_vs_gateway(void) {
    fake_link fl; spy_t y; ipv v;
    mt_stack *s = new_stack(&fl, &y);
    mt_arp_insert(s, T_PEER, T_PEER_MAC, 0);
    mt_arp_insert(s, T_GW, T_GW_MAC, 0);
    memset(mt_ip_payload(s), 0x55, 10);
    T_EQ(mt_ip_output(s, T_PEER, 253, 10, 0), MT_OK);
    T_EQ(fl.n, 1);
    T_CHECK(view_ip(fl.f[0], fl.len[0], &v));
    T_MEMEQ(v.eth_dst, T_PEER_MAC, 6);
    T_EQ(v.dst, T_PEER); T_EQ(v.src, T_ME); T_EQ(v.proto, 253); T_EQ(v.len, 10);
    T_EQ(mt_ip_output(s, T_FAR, 253, 10, 0), MT_OK);
    T_EQ(fl.n, 2);
    T_CHECK(view_ip(fl.f[1], fl.len[1], &v));
    T_MEMEQ(v.eth_dst, T_GW_MAC, 6);
    T_EQ(v.dst, T_FAR);
    T_EQ(mt_get_stats(s)->ip_out, 2);
    /* no gateway: off-link is unroutable */
    {
        mt_ifconfig c; mt_get_config(s, &c); c.gateway = 0; mt_set_config(s, &c);
        T_EQ(mt_ip_output(s, T_FAR, 253, 10, 0), MT_ERR_NOROUTE);
        T_EQ(fl.n, 2);
        c.addr = 0; mt_set_config(s, &c);
        T_EQ(mt_ip_output(s, T_PEER, 253, 10, 0), MT_ERR_UNCONFIGURED);
    }
}

static void test_ip_broadcast(void) {
    fake_link fl; spy_t y; ipv v;
    mt_stack *s = new_stack(&fl, &y);
    T_EQ(mt_ip_output(s, 0xFFFFFFFFu, 17, 8, 0), MT_OK);
    T_EQ(mt_ip_output(s, 0xC0A832FFu, 17, 8, 0), MT_OK);
    T_EQ(fl.n, 2);                       /* no ARP request in between */
    T_CHECK(view_ip(fl.f[0], fl.len[0], &v)); T_MEMEQ(v.eth_dst, BCAST, 6);
    T_CHECK(view_ip(fl.f[1], fl.len[1], &v)); T_MEMEQ(v.eth_dst, BCAST, 6);
    T_EQ(v.dst, 0xC0A832FFu);
}

static void test_ip_header_fields(void) {
    fake_link fl; spy_t y;
    mt_stack *s = new_stack(&fl, &y);
    uint16_t id0;
    mt_arp_insert(s, T_PEER, T_PEER_MAC, 0);
    T_EQ(mt_ip_output(s, T_PEER, 6, 12, 0), MT_OK);
    T_EQ(mt_ip_output(s, T_PEER, 6, 12, 0), MT_OK);
    T_EQ(fl.n, 2);
    T_EQ(fl.f[0][14], 0x45);
    T_EQ(fl.f[0][14 + 8], 64);
    T_EQ(fl.f[0][14 + 9], 6);
    T_EQ(mt_rd16(fl.f[0] + 14 + 2), 32);
    T_EQ(mt_rd16(fl.f[0] + 14 + 6) & 0x4000, 0);      /* DF clear */
    T_EQ(mt_rd16(fl.f[0] + 14 + 6) & 0x3FFF, 0);
    T_EQ(mt_cksum_fold(mt_cksum_add(0, fl.f[0] + 14, 20)), 0);
    T_EQ(mt_cksum_fold(mt_cksum_add(0, fl.f[1] + 14, 20)), 0);
    id0 = mt_rd16(fl.f[0] + 14 + 4);
    T_EQ(mt_rd16(fl.f[1] + 14 + 4), (uint16_t)(id0 + 1));
    T_EQ(mt_ip_src_for(s), T_ME);
}

static void test_ip_fragment_out(void) {
    fake_link fl; spy_t y; static uint8_t pl[3000], re[3000];
    mt_stack *s = new_stack(&fl, &y);
    int i; uint16_t got = 0;
    mt_arp_insert(s, T_PEER, T_PEER_MAC, 0);
    fill(pl, 3000);
    T_EQ(mt_ip_output_buf(s, T_PEER, 253, pl, 3000, 0), MT_OK);
    T_EQ(fl.n, 3);
    for (i = 0; i < 3 && i < fl.n; i++) {
        ipv v; uint16_t ff = mt_rd16(fl.f[i] + 14 + 6);
        T_CHECK(fl.len[i] <= 1514);
        T_CHECK(view_ip(fl.f[i], fl.len[i], &v));
        T_EQ(mt_cksum_fold(mt_cksum_add(0, fl.f[i] + 14, 20)), 0);
        T_EQ(v.frag * 8, i * 1480);                  /* offsets 0 / 1480 / 2960 */
        T_EQ(ff & 0x2000, i < 2 ? 0x2000 : 0);
        T_EQ(v.id, mt_rd16(fl.f[0] + 14 + 4));
        memcpy(re + v.frag * 8, v.payload, v.len);
        got = (uint16_t)(got + v.len);
    }
    T_EQ(got, 3000);
    T_MEMEQ(re, pl, 3000);
    T_EQ(mt_get_stats(s)->ip_frag_out, 3);
    T_EQ(mt_get_stats(s)->ip_out, 1);
    /* in-place fragmentation (payload built at mt_ip_payload) with a smaller mtu */
    {
        mt_ifconfig c; mt_get_config(s, &c); c.mtu = 600; mt_set_config(s, &c);
        fake_clear(&fl);
        memcpy(mt_ip_payload(s), pl, 1000);
        T_EQ(mt_ip_output(s, T_PEER, 253, 1000, 0), MT_OK);
        T_EQ(fl.n, 2);
        T_EQ(fl.len[0], 14 + 20 + 576);
        T_MEMEQ(fl.f[1] + 34, pl + 576, 424);
    }
}

static void test_ip_reassemble_out_of_order(void) {
    fake_link fl; spy_t y; static uint8_t pl[3000];
    mt_stack *s = new_stack(&fl, &y);
    static const int order[3] = {3, 1, 2};
    fill(pl, 3000);
    send_frags(s, pl, 77, order, 3, 0);
    T_EQ(y.n, 1);
    T_EQ(y.len, 3000);
    T_EQ(y.proto, 253); T_EQ(y.src, T_PEER); T_EQ(y.dst, T_ME);
    T_MEMEQ(y.last, pl, 3000);
    T_EQ(mt_get_stats(s)->ip_reasm_ok, 1);
    T_EQ(mt_get_stats(s)->ip_in_drops, 0);
}

static void test_ip_reassemble_timeout(void) {
    fake_link fl; spy_t y; static uint8_t pl[3000];
    mt_stack *s = new_stack(&fl, &y);
    static const int part[2] = {1, 2}, all[3] = {1, 2, 3};
    fill(pl, 3000);
    send_frags(s, pl, 5, part, 2, 1000);
    T_EQ(mt_next_deadline(s, 1000), 15000);
    mt_tick(s, 15999);
    T_EQ(mt_get_stats(s)->ip_reasm_fail, 0);
    mt_tick(s, 16000);
    T_EQ(mt_get_stats(s)->ip_reasm_fail, 1);
    T_EQ(mt_next_deadline(s, 16000), MT_NO_DEADLINE);
    send_frags(s, pl, 6, all, 3, 17000);
    T_EQ(y.n, 1);
    T_EQ(y.len, 3000);
    T_MEMEQ(y.last, pl, 3000);
    /* a second datagram's fragment while the slot is busy is refused */
    send_frags(s, pl, 7, part, 2, 18000);
    send_frags(s, pl, 8, part, 1, 18000);
    T_EQ(mt_get_stats(s)->ip_reasm_fail, 2);
    T_EQ(mt_get_stats(s)->ip_in_drops, 1);
}

static void test_ip_reassemble_too_big(void) {
    fake_link fl; spy_t y; uint8_t f[400], pl[200];
    mt_stack *s = new_stack(&fl, &y);
    memset(pl, 1, sizeof pl);
    /* offset 4000 + 200 > 4096 */
    mt_input(s, f, mk_ip(f, T_PEER, T_ME, 253, 9, (uint16_t)(500 | 0x2000), pl, 200), 0);
    T_EQ(y.n, 0);
    T_EQ(mt_get_stats(s)->ip_reasm_fail, 1);
    T_EQ(mt_get_stats(s)->ip_in_drops, 1);
    /* MF fragment whose length is not a multiple of 8 */
    mt_input(s, f, mk_ip(f, T_PEER, T_ME, 253, 9, 0x2000, pl, 13), 0);
    T_EQ(y.n, 0);
    T_EQ(mt_get_stats(s)->ip_in_drops, 2);
}

static void test_ip_reassemble_overlap(void) {
    fake_link fl; spy_t y; uint8_t f[400], pl[64];
    mt_stack *s = new_stack(&fl, &y);
    memset(pl, 2, sizeof pl);
    mt_input(s, f, mk_ip(f, T_PEER, T_ME, 253, 3, 0x2000, pl, 64), 0);
    mt_input(s, f, mk_ip(f, T_PEER, T_ME, 253, 3, (uint16_t)(4 | 0x2000), pl, 64), 0);   /* overlaps */
    T_EQ(mt_get_stats(s)->ip_reasm_fail, 1);
    T_EQ(mt_get_stats(s)->ip_in_drops, 1);
    /* slot is free again and a clean datagram completes */
    mt_input(s, f, mk_ip(f, T_PEER, T_ME, 253, 4, 0x2000, pl, 64), 0);
    mt_input(s, f, mk_ip(f, T_PEER, T_ME, 253, 4, 8, pl, 10), 0);
    T_EQ(y.n, 1); T_EQ(y.len, 74);
}

static void test_ip_unfragmented_dispatch(void) {
    fake_link fl; spy_t y; uint8_t f[200], pl[100];
    mt_stack *s = new_stack(&fl, &y);
    fill(pl, 100);
    inject(s, f, mk_ip(f, T_PEER, T_ME, 17, 1, 0x4000, pl, 100), 0);      /* DF set */
    T_EQ(y.n, 1); T_EQ(y.proto, 17); T_EQ(y.len, 100); T_MEMEQ(y.last, pl, 100);
    inject(s, f, mk_ip(f, T_PEER, 0xFFFFFFFFu, 17, 2, 0, pl, 4), 0);
    inject(s, f, mk_ip(f, T_PEER, 0xC0A832FFu, 17, 3, 0, pl, 4), 0);
    T_EQ(y.n, 3);
    T_EQ(mt_get_stats(s)->ip_in, 3);
    /* Ethernet padding beyond total length is ignored */
    {
        uint16_t n = mk_ip(f, T_PEER, T_ME, 6, 4, 0, pl, 10);
        memset(f + n, 0, 16);
        inject(s, f, (uint16_t)(n + 16), 0);
        T_EQ(y.n, 4); T_EQ(y.len, 10);
    }
    /* IP options are skipped */
    {
        uint8_t g[200];
        uint16_t n = mk_ip(g, T_PEER, T_ME, 6, 5, 0, pl, 10);
        memmove(g + 14 + 24, g + 14 + 20, 10);
        memset(g + 14 + 20, 1, 4);
        g[14] = 0x46; mt_wr16(g + 14 + 2, 34); mt_wr16(g + 14 + 10, 0);
        mt_wr16(g + 14 + 10, mt_cksum_fold(mt_cksum_add(0, g + 14, 24)));
        inject(s, g, (uint16_t)(n + 4), 0);
        T_EQ(y.n, 5); T_EQ(y.len, 10); T_MEMEQ(y.last, pl, 10);
    }
}

static void test_malformed_table(void) {
    fake_link fl; spy_t y; uint8_t f[200], pl[40];
    mt_stack *s = new_stack(&fl, &y);
    uint16_t n; int i;
    uint32_t base_drops;
    memset(pl, 3, sizeof pl);
    for (i = 0; i < 9; i++) {
        n = mk_ip(f, T_PEER, T_ME, 17, 1, 0, pl, 40);
        switch (i) {
        case 0: n = 14 + 13; break;                                  /* 13-byte IP frame */
        case 1: f[14] = 0x44; break;                                 /* IHL 4 */
        case 2: f[14] = 0x4F; n = 14 + 20; break;                    /* IHL 15 in a 20-byte frame */
        case 3: mt_wr16(f + 16, 1000); break;                        /* total length > frame */
        case 4: f[22] ^= 0x01; break;                                /* TTL changed: bad checksum */
        case 5: f[14] = 0x65; break;                                 /* version 6 */
        case 6: mt_wr32(f + 30, T_ME + 1);                           /* dst not ours (re-summed) */
                mt_wr16(f + 24, 0);
                mt_wr16(f + 24, mt_cksum_fold(mt_cksum_add(0, f + 14, 20))); break;
        case 7: mt_wr16(f + 16, 10); break;                          /* total length < header */
        case 8: n = 14 + 19; break;                                  /* truncated header */
        }
        base_drops = mt_get_stats(s)->ip_in_drops;
        mt_input(s, f, n, 0);
        T_EQ(y.n, 0);
        T_EQ(fl.n, 0);
        T_EQ(mt_get_stats(s)->ip_in_drops, base_drops + 1);
    }
    T_EQ(mt_get_stats(s)->ip_in, 0);
    /* unconfigured stack answers nothing */
    {
        fake_link fl2; spy_t y2;
        mt_stack *u = tstack_new(&fl2, 0);
        memset(&y2, 0, sizeof y2); u->ip_spy = spy_cb; u->ip_spy_ctx = &y2;
        n = mk_ip(f, T_PEER, T_ME, 17, 1, 0, pl, 40);
        mt_input(u, f, n, 0);
        T_EQ(y2.n, 0);
        T_EQ(mt_get_stats(u)->ip_in_drops, 1);
    }
}

typedef struct { int n; uint8_t proto; mt_ip4 src; uint16_t len; uint32_t hash; } sig_t;

static void replay(int odd, sig_t *sig, mt_stats *st) {
    static uint8_t buf[1530];
    fake_link fl; spy_t y;
    mt_stack *s = new_stack(&fl, &y);
    int i;
    for (i = 0; i < rec_n; i++) {
        uint8_t *p = buf + (odd ? 1 : 0);
        memcpy(p, rec_f[i], rec_len[i]);
        mt_tick(s, rec_now[i]);
        mt_input(s, p, rec_len[i], rec_now[i]);
    }
    sig->n = y.n; sig->proto = y.proto; sig->src = y.src; sig->len = y.len; sig->hash = y.hash;
    *st = *mt_get_stats(s);
}

static void test_ip_odd_alignment(void) {
    sig_t a, b; mt_stats sa, sb;
    fake_link fl; spy_t y; static uint8_t pl[3000]; uint8_t f[200];
    mt_stack *s = new_stack(&fl, &y);
    static const int order[3] = {2, 3, 1};
    fill(pl, 3000);
    send_frags(s, pl, 90, order, 3, 30000);
    inject(s, f, mk_ip(f, T_PEER, T_ME, 6, 1, 0, pl, 33), 30000);
    inject(s, f, mk_ip(f, T_GW, 0xFFFFFFFFu, 17, 2, 0, pl, 7), 30000);
    T_CHECK(rec_n >= 5);
    replay(0, &a, &sa);
    replay(1, &b, &sb);
    T_CHECK(a.n >= 5);
    T_EQ(a.n, b.n); T_EQ(a.proto, b.proto); T_EQ(a.src, b.src); T_EQ(a.len, b.len);
    T_EQ(a.hash, b.hash);
    T_MEMEQ(&sa, &sb, sizeof sa);
    /* output from an odd-addressed payload source */
    {
        fake_link fo; spy_t yo; mt_stack *o = new_stack(&fo, &yo);
        mt_arp_insert(o, T_PEER, T_PEER_MAC, 0);
        T_EQ(mt_ip_output_buf(o, T_PEER, 253, pl + 1, 2999, 0), MT_OK);
        T_EQ(fo.n, 3);
    }
}

T_MAIN(test_ip_route_onlink_vs_gateway, test_ip_broadcast, test_ip_header_fields,
       test_ip_fragment_out, test_ip_reassemble_out_of_order, test_ip_reassemble_timeout,
       test_ip_reassemble_too_big, test_ip_reassemble_overlap, test_ip_unfragmented_dispatch,
       test_malformed_table, test_ip_odd_alignment)
