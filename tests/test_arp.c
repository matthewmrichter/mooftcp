#include "t.h"
#include "net.h"
#include "mt_int.h"

static const uint8_t ZERO[6] = {0};
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void inject_reply(mt_stack *s, mt_ip4 ip, const uint8_t mac[6], uint32_t now) {
    uint8_t f[64];
    uint16_t n = mk_arp(f, 2, mac, ip, T_ME_MAC, T_ME);
    mt_input(s, f, n, now);
}

static void test_arp_reply(void) {
    fake_link fl; uint8_t f[64]; arpv v;
    mt_stack *s = tstack_new(&fl, T_ME);
    mt_input(s, f, mk_arp(f, 1, T_PEER_MAC, T_PEER, ZERO, T_ME), 0);
    T_EQ(fl.n, 1);
    T_CHECK(view_arp(fl.f[0], fl.len[0], &v));
    T_EQ(v.op, 2);
    T_EQ(v.sip, T_ME); T_MEMEQ(v.smac, T_ME_MAC, 6);
    T_EQ(v.tip, T_PEER); T_MEMEQ(v.tmac, T_PEER_MAC, 6);
    T_MEMEQ(v.eth_dst, T_PEER_MAC, 6);
    T_MEMEQ(v.eth_src, T_ME_MAC, 6);
    T_EQ(mt_get_stats(s)->arp_in, 1);
    T_EQ(mt_get_stats(s)->arp_out, 1);
}

static void test_arp_ignore_other_target(void) {
    fake_link fl; uint8_t f[64]; uint8_t mac[6];
    mt_stack *s = tstack_new(&fl, T_ME);
    mt_input(s, f, mk_arp(f, 1, T_PEER_MAC, T_PEER, ZERO, T_PEER + 1), 0);
    T_EQ(fl.n, 0);
    /* and the sender was not learned from a request that is not for us */
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 0), 0);
}

static void test_arp_resolve_flow(void) {
    fake_link fl; uint8_t mac[6] = {0}; arpv v;
    mt_stack *s = tstack_new(&fl, T_ME);
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 0), 0);
    T_EQ(fl.n, 1);
    T_CHECK(view_arp(fl.f[0], fl.len[0], &v));
    T_EQ(v.op, 1); T_EQ(v.sip, T_ME); T_EQ(v.tip, T_PEER);
    T_MEMEQ(v.smac, T_ME_MAC, 6);
    T_MEMEQ(v.eth_dst, BCAST, 6);
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 500), 0);
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 999), 0);
    T_EQ(fl.n, 1);
    inject_reply(s, T_PEER, T_PEER_MAC, 600);
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 700), 1);
    T_MEMEQ(mac, T_PEER_MAC, 6);
    T_EQ(fl.n, 1);
}

static void test_arp_learn_from_request(void) {
    fake_link fl; uint8_t f[64]; uint8_t mac[6];
    mt_stack *s = tstack_new(&fl, T_ME);
    mt_input(s, f, mk_arp(f, 1, T_PEER_MAC, T_PEER, ZERO, T_ME), 0);
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 1), 1);
    T_MEMEQ(mac, T_PEER_MAC, 6);
}

static void test_arp_pending_frame_sent_on_reply(void) {
    fake_link fl; arpv v; int i;
    mt_stack *s = tstack_new(&fl, T_ME);
    for (i = 0; i < 20; i++) s->txbuf[14 + i] = (uint8_t)(0xA0 + i);
    T_EQ(mt_eth_send(s, T_PEER, 0x0800, 20, 0), 0);
    T_EQ(fl.n, 1);
    T_CHECK(view_arp(fl.f[0], fl.len[0], &v));
    T_EQ(v.op, 1);
    inject_reply(s, T_PEER, T_PEER_MAC, 100);
    T_EQ(fl.n, 2);
    T_EQ(fl.len[1], 34);
    T_MEMEQ(fl.f[1], T_PEER_MAC, 6);
    T_MEMEQ(fl.f[1] + 6, T_ME_MAC, 6);
    T_EQ(mt_rd16(fl.f[1] + 12), 0x0800);
    T_MEMEQ(fl.f[1] + 14, s->txbuf + 14, 20);
    /* now resolved: direct send */
    T_EQ(mt_eth_send(s, T_PEER, 0x0800, 20, 200), 1);
    T_EQ(fl.n, 3);
    T_MEMEQ(fl.f[2], T_PEER_MAC, 6);
}

static void test_arp_pending_other_hop_not_sent(void) {
    fake_link fl;
    mt_stack *s = tstack_new(&fl, T_ME);
    T_EQ(mt_eth_send(s, T_PEER, 0x0800, 20, 0), 0);
    inject_reply(s, T_GW, T_GW_MAC, 100);
    T_EQ(fl.n, 1);                       /* only the request */
    inject_reply(s, T_PEER, T_PEER_MAC, 200);
    T_EQ(fl.n, 2);
}

static void test_arp_pending_expires(void) {
    fake_link fl; uint8_t mac[6];
    mt_stack *s = tstack_new(&fl, T_ME);
    T_EQ(mt_eth_send(s, T_PEER, 0x0800, 20, 0), 0);
    T_EQ(fl.n, 1);
    mt_tick(s, 500);  T_EQ(fl.n, 1);
    mt_tick(s, 1000); T_EQ(fl.n, 2);
    mt_tick(s, 1000); T_EQ(fl.n, 2);     /* no duplicate for the same instant */
    mt_tick(s, 2000); T_EQ(fl.n, 3);
    mt_tick(s, 2999); T_EQ(fl.n, 3);
    T_EQ(mt_get_stats(s)->arp_drops, 0);
    mt_tick(s, 3000); T_EQ(fl.n, 3);
    T_EQ(mt_get_stats(s)->arp_drops, 1);
    T_EQ(mt_get_stats(s)->arp_out, 3);
    inject_reply(s, T_PEER, T_PEER_MAC, 3100);
    T_EQ(fl.n, 3);                       /* held frame was discarded */
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 3200), 1);
}

static void test_arp_lru_replace(void) {
    fake_link fl; uint8_t mac[6]; int i;
    mt_stack *s = tstack_new(&fl, T_ME);
    for (i = 0; i < 8; i++) mt_arp_insert(s, T_PEER + (uint32_t)i, T_PEER_MAC, (uint32_t)i);
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 100), 1);          /* refresh entry 0 */
    mt_arp_insert(s, T_PEER + 8, T_PEER_MAC, 200);
    T_EQ(fl.n, 0);
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 210), 1);
    for (i = 2; i <= 8; i++) T_EQ(mt_arp_resolve(s, T_PEER + (uint32_t)i, mac, 210), 1);
    T_EQ(fl.n, 0);
    T_EQ(mt_arp_resolve(s, T_PEER + 1, mac, 220), 0);      /* evicted: oldest used */
    T_EQ(fl.n, 1);
}

static void test_arp_resolved_ages(void) {
    fake_link fl; uint8_t mac[6];
    mt_stack *s = tstack_new(&fl, T_ME);
    mt_arp_insert(s, T_PEER, T_PEER_MAC, 1000);
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 1000 + 599999), 1);
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 1000 + 600000), 0);
    T_EQ(fl.n, 1);
}

static void test_arp_oversize_not_held(void) {
    fake_link fl; arpv v;
    mt_stack *s = tstack_new(&fl, T_ME);
    T_EQ(mt_eth_send(s, T_PEER, 0x0800, 1500, 0), 0);
    T_EQ(fl.n, 1);
    T_CHECK(view_arp(fl.f[0], fl.len[0], &v));
    T_EQ(v.op, 1);
    T_EQ(mt_get_stats(s)->arp_drops, 1);
    inject_reply(s, T_PEER, T_PEER_MAC, 100);
    T_EQ(fl.n, 1);                       /* nothing held, nothing extra sent */
    T_EQ(mt_get_stats(s)->arp_drops, 1);
    /* a frame exactly at the cap is held */
    T_EQ(mt_eth_send(s, T_PEER + 1, 0x0800, MT_PEND_MAX - 14, 200), 0);
    T_EQ(mt_get_stats(s)->arp_drops, 1);
    inject_reply(s, T_PEER + 1, T_PEER_MAC, 300);
    T_EQ(fl.n, 3);
    T_EQ(fl.len[2], MT_PEND_MAX);
}

static void test_arp_expiry_wrap(void) {
    fake_link fl; uint8_t mac[6];
    mt_stack *s = tstack_new(&fl, T_ME);
    /* stamp just below 2^32: expiry crosses the clock wrap */
    mt_arp_insert(s, T_PEER, T_PEER_MAC, 0xFFFFFFF0u);
    T_EQ(mt_next_deadline(s, 0xFFFFFFF0u), 600000);
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 0xFFFFFFF0u + 599999u), 1);
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 0xFFFFFFF0u + 600000u), 0);
    /* ticked at the 10-minute mark, then idle for > 2^31 ms: must not revive */
    mt_arp_insert(s, T_PEER + 1, T_PEER_MAC, 1000);
    mt_tick(s, 1000 + 600000);
    fl.n = 0;
    T_EQ(mt_arp_resolve(s, T_PEER + 1, mac, 1000 + 0x90000000u), 0);
    T_EQ(fl.n, 1);
}

static void test_deadline(void) {
    fake_link fl; uint8_t mac[6];
    mt_stack *s = tstack_new(&fl, T_ME);
    T_EQ(mt_next_deadline(s, 0), MT_NO_DEADLINE);
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 0), 0);
    T_EQ(mt_next_deadline(s, 0), 1000);
    T_EQ(mt_next_deadline(s, 400), 600);
    T_EQ(mt_next_deadline(s, 1500), 0);                    /* overdue */
    mt_tick(s, 1000);
    T_EQ(mt_next_deadline(s, 1000), 1000);
    mt_tick(s, 2000);
    T_EQ(mt_next_deadline(s, 2000), 1000);                 /* expiry at 3000 */
    mt_tick(s, 3000);
    T_EQ(mt_next_deadline(s, 3000), MT_NO_DEADLINE);
    /* clock wrap */
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 0xFFFFFF00u), 0);
    T_EQ(mt_next_deadline(s, 0xFFFFFF00u), 1000);
}

static void test_arp_odd_address(void) {
    fake_link fl; uint8_t buf[80]; uint8_t f[64]; arpv v;
    mt_stack *s = tstack_new(&fl, T_ME);
    uint16_t n = mk_arp(f, 1, T_PEER_MAC, T_PEER, ZERO, T_ME);
    memcpy(buf + 1, f, n);
    mt_input(s, buf + 1, n, 0);
    T_EQ(fl.n, 1);
    T_CHECK(view_arp(fl.f[0], fl.len[0], &v));
    T_EQ(v.op, 2);
}

static void test_arp_malformed(void) {
    fake_link fl; uint8_t f[64];
    mt_stack *s = tstack_new(&fl, T_ME);
    uint16_t n = mk_arp(f, 1, T_PEER_MAC, T_PEER, ZERO, T_ME);
    mt_input(s, f, 41, 0);                 /* short ARP */
    f[19] = 5;                             /* bad protocol address length */
    mt_input(s, f, n, 0);
    f[19] = 4; mt_wr16(f + 20, 7);         /* unknown opcode */
    mt_input(s, f, n, 0);
    T_EQ(fl.n, 0);
}

T_MAIN(test_arp_reply, test_arp_ignore_other_target, test_arp_resolve_flow,
       test_arp_learn_from_request, test_arp_pending_frame_sent_on_reply,
       test_arp_pending_other_hop_not_sent, test_arp_pending_expires,
       test_arp_lru_replace, test_arp_resolved_ages, test_deadline,
       test_arp_odd_address, test_arp_malformed, test_arp_oversize_not_held,
       test_arp_expiry_wrap)
