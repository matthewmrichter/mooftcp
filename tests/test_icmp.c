#include "t.h"
#include "net.h"
#include "mt_int.h"
#include "mt_util.h"

typedef struct { int n; uint8_t proto, type, code; mt_ip4 dst; uint16_t lport, rport; } espy;
static void err_spy(void *c, uint8_t proto, mt_ip4 dst, uint16_t lp, uint16_t rp, uint8_t type,
                    uint8_t code) {
    espy *e = (espy *)c;
    e->n++; e->proto = proto; e->dst = dst; e->lport = lp; e->rport = rp; e->type = type;
    e->code = code;
}
typedef struct { int n; mt_ip4 from; uint16_t id, seq, len; uint8_t data[64]; } ecb;
static void echo_cb(void *u, mt_ip4 from, uint16_t id, uint16_t seq, const uint8_t *d,
                    uint16_t len, uint32_t now) {
    ecb *e = (ecb *)u;
    (void)now;
    e->n++; e->from = from; e->id = id; e->seq = seq; e->len = len;
    if (len <= sizeof e->data) memcpy(e->data, d, len);
}

static mt_stack *setup(fake_link *fl) {
    mt_stack *s = tstack_new(fl, T_ME);
    mt_arp_insert(s, T_PEER, T_PEER_MAC, 0);
    return s;
}

/* ICMP message with a correct checksum; returns length. */
static uint16_t mk_icmp(uint8_t *m, uint8_t type, uint8_t code, uint16_t id, uint16_t seq,
                        const uint8_t *d, uint16_t n) {
    m[0] = type; m[1] = code; m[2] = 0; m[3] = 0;
    mt_wr16(m + 4, id); mt_wr16(m + 6, seq);
    if (n) memcpy(m + 8, d, n);
    mt_wr16(m + 2, mt_cksum_fold(mt_cksum_add(0, m, (uint32_t)8 + n)));
    return (uint16_t)(8 + n);
}

/* odd != 0 places the frame at an odd address. */
static void send_icmp(mt_stack *s, mt_ip4 src, mt_ip4 dst, const uint8_t *m, uint16_t n,
                      uint32_t odd) {
    static uint8_t buf[1600];
    uint8_t *f = buf + (odd & 1);
    uint16_t fl = mk_ip(f, src, dst, 1, 5, 0, m, n);
    mt_input(s, f, fl, 10);
}

static void pat(uint8_t *d, uint16_t n) {
    uint16_t i;
    for (i = 0; i < n; i++) d[i] = (uint8_t)(i * 3 + 1);
}

static void echo_one(uint16_t n, uint32_t odd) {
    fake_link fl; mt_stack *s = setup(&fl);
    uint8_t d[1480], m[1500]; ipv v; uint16_t ml;
    pat(d, n);
    ml = mk_icmp(m, 8, 0, 7, 9, d, n);
    send_icmp(s, T_PEER, T_ME, m, ml, odd);
    T_EQ(fl.n, 1);
    T_CHECK(view_ip(fl.f[0], fl.len[0], &v));
    T_EQ(v.proto, 1); T_EQ(v.src, T_ME); T_EQ(v.dst, T_PEER);
    T_EQ(v.len, ml);
    T_EQ(v.payload[0], 0); T_EQ(v.payload[1], 0);
    T_EQ(mt_rd16(v.payload + 4), 7); T_EQ(mt_rd16(v.payload + 6), 9);
    T_MEMEQ(v.payload + 8, d, n);
    T_EQ(mt_cksum_fold(mt_cksum_add(0, v.payload, v.len)), 0);
    T_EQ(mt_get_stats(s)->icmp_in, 1); T_EQ(mt_get_stats(s)->icmp_out, 1);
}

static void test_echo_reply(void) { echo_one(56, 0); }
static void test_echo_reply_odd(void) { echo_one(55, 1); echo_one(0, 1); }
static void test_echo_reply_max_one_frame(void) { echo_one(1472, 0); }

static void test_echo_reply_fragmented(void) {  /* 3000-byte echo, reassembled, reply fragmented */
    fake_link fl; mt_stack *s = setup(&fl);
    static uint8_t d[3000 - 8], m[3000], f[1520];
    uint16_t ml, off, n, fln; int i; uint32_t got = 0;
    pat(d, sizeof d);
    ml = mk_icmp(m, 8, 0, 3, 4, d, sizeof d);
    for (off = 0; off < ml; off = (uint16_t)(off + 1480)) {
        n = (uint16_t)(ml - off > 1480 ? 1480 : ml - off);
        fln = mk_ip(f, T_PEER, T_ME, 1, 77, (uint16_t)(off / 8 | (off + n < ml ? 0x2000 : 0)),
                    m + off, n);
        mt_input(s, f, fln, 10);
    }
    T_EQ(fl.n, 3);
    for (i = 0; i < fl.n; i++) {
        ipv v; T_CHECK(view_ip(fl.f[i], fl.len[i], &v));
        if (i == 0) { T_EQ(v.payload[0], 0); T_EQ(mt_rd16(v.payload + 4), 3); }
        got += v.len;
    }
    T_EQ(got, ml);
}

static void test_echo_bad_cksum_dropped(void) {
    fake_link fl; mt_stack *s = setup(&fl);
    uint8_t d[56], m[64]; uint16_t ml;
    pat(d, 56);
    ml = mk_icmp(m, 8, 0, 7, 9, d, 56);
    m[20] ^= 0x40;
    send_icmp(s, T_PEER, T_ME, m, ml, 0);
    T_EQ(fl.n, 0);
    T_EQ(mt_get_stats(s)->icmp_in, 0);
    T_EQ(mt_get_stats(s)->ip_in_drops, 1);
}

static void test_icmp_truncated_dropped(void) {
    fake_link fl; mt_stack *s = setup(&fl);
    uint8_t m[8];
    (void)mk_icmp(m, 8, 0, 1, 1, NULL, 0);
    send_icmp(s, T_PEER, T_ME, m, 7, 0);
    T_EQ(fl.n, 0);
    T_EQ(mt_get_stats(s)->ip_in_drops, 1);
}

static void test_echo_request_and_cb(void) {
    fake_link fl; mt_stack *s = setup(&fl);
    uint8_t m[16]; ecb e; ipv v; uint16_t ml;
    memset(&e, 0, sizeof e);
    mt_set_echo_cb(s, echo_cb, &e);
    T_EQ(mt_icmp_echo(s, T_PEER, 1, 2, (const uint8_t *)"hi", 2, 5), MT_OK);
    T_EQ(fl.n, 1);
    T_CHECK(view_ip(fl.f[0], fl.len[0], &v));
    T_EQ(v.proto, 1); T_EQ(v.payload[0], 8); T_EQ(v.len, 10);
    T_EQ(mt_cksum_fold(mt_cksum_add(0, v.payload, v.len)), 0);
    T_EQ(mt_icmp_echo(s, T_PEER, 1, 2, NULL, 1473, 5), MT_ERR_INVAL);
    ml = mk_icmp(m, 0, 0, 1, 2, (const uint8_t *)"hi", 2);
    send_icmp(s, T_PEER, T_ME, m, ml, 1);
    T_EQ(e.n, 1); T_EQ(e.from, T_PEER); T_EQ(e.id, 1); T_EQ(e.seq, 2);
    T_EQ(e.len, 2); T_MEMEQ(e.data, "hi", 2);
    T_EQ(fl.n, 1);   /* a reply is never answered */
}

static void test_unreach_dispatch(void) {
    fake_link fl; mt_stack *s = setup(&fl);
    uint8_t q[32], m[8 + 32]; espy e; uint16_t ml;
    memset(&e, 0, sizeof e);
    s->icmp_err_spy = err_spy; s->icmp_err_ctx = &e;
    /* original datagram: T_ME -> T_FAR, TCP 49152 -> 23 */
    memset(q, 0, sizeof q);
    q[0] = 0x45; mt_wr16(q + 2, 60); q[8] = 64; q[9] = 6;
    mt_wr32(q + 12, T_ME); mt_wr32(q + 16, T_FAR);
    mt_wr16(q + 20, 49152); mt_wr16(q + 22, 23);
    ml = mk_icmp(m, 3, 1, 0, 0, q, 28);
    send_icmp(s, T_GW, T_ME, m, ml, 0);
    T_EQ(e.n, 1); T_EQ(e.proto, 6); T_EQ(e.dst, T_FAR); T_EQ(e.lport, 49152);
    T_EQ(e.rport, 23); T_EQ(e.type, 3); T_EQ(e.code, 1);
    /* UDP, time exceeded, with IHL 6 (options): ports follow 24 header bytes */
    q[0] = 0x46; q[9] = 17;
    memmove(q + 24, q + 20, 4);
    memset(q + 20, 0, 4);
    ml = mk_icmp(m, 11, 0, 0, 0, q, 32);
    send_icmp(s, T_GW, T_ME, m, ml, 1);
    T_EQ(e.n, 2); T_EQ(e.proto, 17); T_EQ(e.type, 11); T_EQ(e.lport, 49152);
    /* too short for IHL*4+8: ignored */
    ml = mk_icmp(m, 3, 3, 0, 0, q, 31);
    send_icmp(s, T_GW, T_ME, m, ml, 0);
    /* not our datagram (original src != us): ignored */
    q[0] = 0x45; memmove(q + 20, q + 24, 4); mt_wr32(q + 12, T_PEER);
    ml = mk_icmp(m, 3, 3, 0, 0, q, 28);
    send_icmp(s, T_GW, T_ME, m, ml, 0);
    /* unknown embedded protocol and embedded ICMP: ignored */
    mt_wr32(q + 12, T_ME); q[9] = 47;
    ml = mk_icmp(m, 3, 3, 0, 0, q, 28);
    send_icmp(s, T_GW, T_ME, m, ml, 0);
    q[9] = 1;
    ml = mk_icmp(m, 3, 3, 0, 0, q, 28);
    send_icmp(s, T_GW, T_ME, m, ml, 0);
    T_EQ(e.n, 2);
    T_EQ(fl.n, 0);
}

static void test_no_reply_to_broadcast_echo(void) {
    fake_link fl; mt_stack *s = setup(&fl);
    uint8_t m[16]; uint16_t ml = mk_icmp(m, 8, 0, 1, 1, (const uint8_t *)"abcdefgh", 8);
    send_icmp(s, T_PEER, 0xFFFFFFFFu, m, ml, 0);
    send_icmp(s, T_PEER, 0xC0A832FFu, m, ml, 0);   /* directed broadcast */
    T_EQ(fl.n, 0);
}

static void test_no_reply_to_bad_source(void) {
    fake_link fl; mt_stack *s = setup(&fl);
    uint8_t m[16]; uint16_t ml = mk_icmp(m, 8, 0, 1, 1, (const uint8_t *)"abcdefgh", 8);
    send_icmp(s, 0, T_ME, m, ml, 0);
    send_icmp(s, 0xFFFFFFFFu, T_ME, m, ml, 0);
    send_icmp(s, 0xC0A832FFu, T_ME, m, ml, 0);
    send_icmp(s, 0xE0000001u, T_ME, m, ml, 0);
    send_icmp(s, T_ME, T_ME, m, ml, 0);
    T_EQ(fl.n, 0);
}

static void test_unconfigured_silent(void) {
    fake_link fl; mt_stack *s = tstack_new(&fl, 0);
    uint8_t m[16]; uint16_t ml = mk_icmp(m, 8, 0, 1, 1, (const uint8_t *)"abcdefgh", 8);
    send_icmp(s, T_PEER, T_ME, m, ml, 0);
    T_EQ(fl.n, 0);
}

static void test_send_unreach(void) {
    fake_link fl; mt_stack *s = setup(&fl);
    uint8_t udp[12] = {0xC0, 0, 0, 53, 0, 12, 0, 0, 1, 2, 3, 4}; ipv v;
    mt_icmp_send_unreach(s, T_PEER, 0xFFFFFFFFu, 17, udp, 12, 3, 1);
    mt_icmp_send_unreach(s, T_PEER, 0xC0A832FFu, 17, udp, 12, 3, 1);
    mt_icmp_send_unreach(s, 0xFFFFFFFFu, T_ME, 17, udp, 12, 3, 1);
    mt_icmp_send_unreach(s, T_ME, T_ME, 17, udp, 12, 3, 1);
    T_EQ(fl.n, 0);
    mt_icmp_send_unreach(s, T_PEER, T_ME, 17, udp, 12, 3, 1);
    T_EQ(fl.n, 1);
    T_CHECK(view_ip(fl.f[0], fl.len[0], &v));
    T_EQ(v.proto, 1); T_EQ(v.dst, T_PEER); T_EQ(v.len, 8 + 20 + 8);
    T_EQ(v.payload[0], 3); T_EQ(v.payload[1], 3);
    T_EQ(mt_cksum_fold(mt_cksum_add(0, v.payload, v.len)), 0);
    T_EQ(mt_rd32(v.payload + 8 + 12), T_PEER); T_EQ(mt_rd32(v.payload + 8 + 16), T_ME);
    T_EQ(v.payload[8 + 9], 17);
    T_MEMEQ(v.payload + 8 + 20, udp, 8);
    T_EQ(mt_get_stats(s)->icmp_out, 1);
}

T_MAIN(test_echo_reply, test_echo_reply_odd, test_echo_reply_max_one_frame,
       test_echo_reply_fragmented, test_echo_bad_cksum_dropped, test_icmp_truncated_dropped,
       test_echo_request_and_cb, test_unreach_dispatch, test_no_reply_to_broadcast_echo,
       test_no_reply_to_bad_source, test_unconfigured_silent, test_send_unreach)
