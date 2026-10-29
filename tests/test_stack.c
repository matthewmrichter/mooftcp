#include "t.h"
#include "net.h"
#include "mt_int.h"

static void test_stack_size_budget(void) {
    static uint64_t mem[16384 / 8 + 8];
    mt_limits l;
    mt_link link = {0, 0};
    uint32_t sz;
    mt_limits_default(&l);
    T_EQ(l.max_tcp, 16); T_EQ(l.max_udp, 8); T_EQ(l.arp_entries, 8); T_EQ(l.dns_cache, 8);
    sz = mt_stack_size(&l);
    T_CHECK(sz <= 16384);
    T_CHECK(mt_stack_init(mem, sz, &l, &link, 1) != NULL);
    T_CHECK(mt_stack_init(mem, sz - 1, &l, &link, 1) == NULL);
    T_CHECK(mt_stack_init((uint8_t *)mem + 2, sz, &l, &link, 1) == NULL);  /* misaligned */
}

static void test_unconfigured_silent(void) {
    fake_link fl; uint8_t f[64];
    static const uint8_t zero[6] = {0};
    mt_stack *s = tstack_new(&fl, 0);
    uint16_t n = mk_arp(f, 1, T_PEER_MAC, T_PEER, zero, T_ME);
    mt_input(s, f, n, 0);
    n = mk_arp(f, 1, T_PEER_MAC, T_PEER, zero, 0);
    mt_input(s, f, n, 0);
    T_EQ(fl.n, 0);
    T_EQ(mt_arp_resolve(s, T_PEER, f, 0), 0);
    T_CHECK(mt_eth_send(s, T_PEER, 0x0800, 10, 0) <= 0);
    mt_tick(s, 5000);
    T_EQ(fl.n, 0);
    T_EQ(mt_next_deadline(s, 5000), MT_NO_DEADLINE);
}

static void test_demux_filters(void) {
    fake_link fl; uint8_t f[64];
    static const uint8_t zero[6] = {0};
    mt_stack *s = tstack_new(&fl, T_ME);
    uint16_t n = mk_arp(f, 1, T_PEER_MAC, T_PEER, zero, T_ME);
    mt_input(s, f, 13, 0);                         /* runt */
    f[5] = 0x77;                                   /* unicast to someone else */
    mt_input(s, f, n, 0);
    T_EQ(fl.n, 0);
    f[0] = 0x01; f[1] = 0; f[2] = 0x5E; f[3] = 0; f[4] = 0; f[5] = 1;  /* multicast: not ours */
    mt_input(s, f, n, 0);
    T_EQ(fl.n, 0);
    memcpy(f, T_ME_MAC, 6);                        /* unicast to us is accepted */
    mt_input(s, f, n, 0);
    T_EQ(fl.n, 1);
    T_EQ(mt_get_stats(s)->eth_in, 1);
    T_EQ(mt_get_stats(s)->eth_out, 1);
    T_EQ(mt_get_stats(s)->arp_in, 1);
    T_EQ(mt_get_stats(s)->arp_out, 1);
    /* IPv4 reaches the (stub) IP layer without output */
    n = mk_ip(f, T_PEER, T_ME, 17, 1, 0, (const uint8_t *)"abcd", 4);
    mt_input(s, f, n, 0);
    T_EQ(fl.n, 1);
}

static void test_tx_error_counted(void) {
    fake_link fl; uint8_t f[64];
    static const uint8_t zero[6] = {0};
    mt_stack *s = tstack_new(&fl, T_ME);
    uint16_t n = mk_arp(f, 1, T_PEER_MAC, T_PEER, zero, T_ME);
    int i;
    for (i = 0; i < 65; i++) mt_input(s, f, n, 0);   /* fake link refuses after 64 */
    T_EQ(mt_get_stats(s)->eth_out, 64);
    T_EQ(mt_get_stats(s)->eth_tx_err, 1);
}

T_MAIN(test_stack_size_budget, test_unconfigured_silent, test_demux_filters, test_tx_error_counted)
