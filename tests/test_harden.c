#include "t.h"
#include "net.h"
#include "mt_int.h"
#include "mt_util.h"

/* Plan 2 Task 5: cheap core hardening (carry-over list). */

static const uint8_t MSS1460[4] = {2, 4, 0x05, 0xB4};
static const uint8_t MAC2[6] = {0x02, 0, 0, 0, 0, 0x77};
static const uint8_t ZERO[6] = {0};

static mt_stack *setup(fake_link *fl) {
    mt_stack *s = tstack_new(fl, T_ME);
    mt_arp_insert(s, T_PEER, T_PEER_MAC, 0);
    return s;
}

static void inject_from(mt_stack *s, mt_ip4 src, uint16_t sport, uint16_t dport, uint32_t seq,
                        uint32_t ack, uint8_t flags, const uint8_t *opt, uint8_t optlen,
                        const uint8_t *data, uint16_t dlen, uint32_t now) {
    static uint8_t buf[1700];
    uint8_t *f = buf + 1;
    uint16_t n = mk_tcp(f, src, sport, T_ME, dport, seq, ack, flags, 8192, opt, optlen, data,
                        dlen);
    mt_input(s, f, n, now);
}

static void inject(mt_stack *s, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                   uint8_t flags, const uint8_t *data, uint16_t dlen, uint32_t now) {
    inject_from(s, T_PEER, sport, dport, seq, ack, flags,
                (flags & TF_SYN) ? MSS1460 : NULL, (flags & TF_SYN) ? 4 : 0, data, dlen, now);
}

static int last_tcp(const fake_link *fl, tcpv *v) {
    return fl->n > 0 && view_tcp(fl->f[fl->n - 1], fl->len[fl->n - 1], v);
}

static void inject_udp(mt_stack *s, mt_ip4 src, uint16_t sport, uint16_t dport, uint32_t now) {
    uint8_t f[100], u[12];
    uint16_t n;
    mt_wr16(u, sport); mt_wr16(u + 2, dport); mt_wr16(u + 4, 12); mt_wr16(u + 6, 0);
    u[8] = 'a'; u[9] = 'b'; u[10] = 'c'; u[11] = 'd';
    n = mk_ip(f, src, T_ME, 17, 5, 0, u, 12);
    mt_input(s, f, n, now);
}

static void inject_echo(mt_stack *s, mt_ip4 src, uint32_t now) {
    uint8_t f[100], p[8] = {8, 0, 0, 0, 0, 1, 0, 1};
    uint16_t n;
    mt_wr16(p + 2, mt_cksum_fold(mt_cksum_add(0, p, 8)));
    n = mk_ip(f, src, T_ME, 1, 9, 0, p, 8);
    mt_input(s, f, n, now);
}

/* ---- ARP ---- */

static void test_arp_merge_updates_existing_from_any_arp(void) {
    fake_link fl; uint8_t f[64], mac[6];
    mt_stack *s = tstack_new(&fl, T_ME);
    uint16_t n;
    mt_arp_insert(s, T_GW, T_GW_MAC, 0);
    /* gateway's MAC changed; its request is for somebody else: no reply, but the entry follows */
    n = mk_arp(f, 1, MAC2, T_GW, ZERO, T_PEER);
    mt_input(s, f, n, 1000);
    T_EQ(fl.n, 0);
    T_EQ(mt_arp_resolve(s, T_GW, mac, 1000), 1);
    T_MEMEQ(mac, MAC2, 6);
    /* the same through a reply to a third party (eth dst is us so it reaches the stack) */
    n = mk_arp(f, 2, T_GW_MAC, T_GW, T_ME_MAC, T_PEER);
    mt_input(s, f, n, 2000);
    T_EQ(mt_arp_resolve(s, T_GW, mac, 2000), 1);
    T_MEMEQ(mac, T_GW_MAC, 6);
    /* the lifetime is refreshed too: inserted at 2000, refreshed at 500000, alive at 1000000 */
    n = mk_arp(f, 1, T_GW_MAC, T_GW, ZERO, T_PEER);
    mt_input(s, f, n, 500000);
    T_EQ(mt_arp_resolve(s, T_GW, mac, 1000000), 1);
    T_EQ(fl.n, 0);
    /* a sender we have no entry for is not learned unless the request is for us */
    n = mk_arp(f, 1, MAC2, T_PEER + 1, ZERO, T_PEER);
    mt_input(s, f, n, 1000000);
    T_EQ(mt_arp_resolve(s, T_PEER + 1, mac, 1000000), 0);   /* pending: a request goes out */
    T_EQ(fl.n, 1);
    fake_clear(&fl);
    n = mk_arp(f, 1, MAC2, T_PEER + 2, ZERO, T_ME);          /* addressed to us: learn and reply */
    mt_input(s, f, n, 1000000);
    T_EQ(fl.n, 1);
    T_EQ(mt_arp_resolve(s, T_PEER + 2, mac, 1000000), 1);
}

static void test_arp_ignores_multicast_sender_and_own_mac(void) {
    fake_link fl; uint8_t f[64], mac[6];
    static const uint8_t MCAST[6] = {0x01, 0x00, 0x5E, 0, 0, 1};
    static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    mt_stack *s = setup(&fl);
    uint16_t n;
    /* multicast / broadcast sender hardware address: no reply, no learning, no overwrite */
    n = mk_arp(f, 1, MCAST, T_PEER + 3, ZERO, T_ME);
    mt_input(s, f, n, 10);
    n = mk_arp(f, 1, BCAST, T_PEER + 4, ZERO, T_ME);
    mt_input(s, f, n, 10);
    n = mk_arp(f, 1, MCAST, T_PEER, ZERO, T_ME);
    mt_input(s, f, n, 10);
    T_EQ(fl.n, 0);
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 10), 1);
    T_MEMEQ(mac, T_PEER_MAC, 6);
    T_EQ(mt_arp_resolve(s, T_PEER + 3, mac, 10), 0);
    fake_clear(&fl);
    /* sender hardware address is our own */
    n = mk_arp(f, 1, T_ME_MAC, T_PEER, ZERO, T_ME);
    mt_input(s, f, n, 20);
    T_EQ(fl.n, 0);
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 20), 1);
    T_MEMEQ(mac, T_PEER_MAC, 6);
    /* Ethernet frame sourced from our own MAC (reflection), ARP and IP */
    n = mk_arp(f, 1, MAC2, T_PEER, ZERO, T_ME);
    memcpy(f + 6, T_ME_MAC, 6);
    mt_input(s, f, n, 30);
    T_EQ(fl.n, 0);
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 30), 1);
    T_MEMEQ(mac, T_PEER_MAC, 6);
    { uint8_t g[100], p[8] = {8, 0, 0, 0, 0, 1, 0, 1};
      mt_wr16(p + 2, mt_cksum_fold(mt_cksum_add(0, p, 8)));
      n = mk_ip(g, T_PEER, T_ME, 1, 9, 0, p, 8);
      memcpy(g + 6, T_ME_MAC, 6);
      mt_input(s, g, n, 40);
      T_EQ(fl.n, 0);
      T_EQ(mt_get_stats(s)->icmp_in, 0); }
}

/* ---- IP source validation ---- */

static void test_ip_rejects_multicast_own_and_subnet_broadcast_source(void) {
    fake_link fl; mt_logrec lr[16]; uint16_t k, nd = 0;
    static const mt_ip4 bad[] = {0xE0000005u, 0xEFFFFFFFu, 0xF0000001u, 0xFFFFFFFFu, T_ME,
                                 0xC0A832FFu};
    mt_stack *s = setup(&fl);
    uint32_t i, in0;
    mt_log_enable(s, 1);
    in0 = mt_get_stats(s)->ip_in;
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        inject_echo(s, bad[i], 10 + i);
        T_EQ(mt_get_stats(s)->ip_in_drops, i + 1);
    }
    T_EQ(fl.n, 0);
    T_EQ(mt_get_stats(s)->ip_in, in0);                 /* refused as header failures */
    k = mt_log_read(s, lr, 16);
    for (i = 0; i < k; i++)
        if (lr[i].code == MT_LOG_DROP && lr[i].a == MT_LOGL_IP) nd++;
    T_EQ(nd, sizeof bad / sizeof bad[0]);              /* each logged with its reason line */
    inject_echo(s, T_PEER, 100);                       /* control: a unicast peer is answered */
    T_EQ(fl.n, 1);
    T_EQ(mt_get_stats(s)->ip_in, in0 + 1);
}

static void test_tcp_syn_from_multicast_no_rst(void) {
    fake_link fl;
    mt_stack *s = setup(&fl);
    static uint8_t b[4096];
    mt_tcp *t = mt_tcp_create(s, b, sizeof b, NULL, NULL);
    mt_tcp_info in;
    T_EQ(mt_tcp_listen(t, 0, 0, 21), MT_OK);
    inject_from(s, 0xE0000005u, 1025, 99, 100, 0, TF_SYN, MSS1460, 4, NULL, 0, 1);   /* closed port */
    inject_from(s, 0xE0000005u, 1025, 21, 100, 0, TF_SYN, MSS1460, 4, NULL, 0, 2);   /* listener */
    T_EQ(fl.n, 0);                                     /* no RST, no SYN-ACK, no ARP request */
    mt_tcp_get_info(t, &in);
    T_EQ(in.state, MT_TCP_LISTEN);
}

/* ---- TCP ---- */

static void test_rst_exact_match_only(void) {
    fake_link fl; rec r; tcpv v; mt_tcp_info in; uint32_t ps, ms;
    mt_stack *s = setup(&fl);
    mt_tcp *t;
    memset(&r, 0, sizeof r);
    t = tconn_established(s, &fl, &r, &ps, &ms);
    mt_tcp_get_info(t, &in);
    inject(s, 23, in.lport, ps + 10, 0, TF_RST, NULL, 0, 10);       /* in window, not exact */
    T_EQ(r.n, 1);                                                    /* ESTABLISHED only */
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_ESTABLISHED);
    T_EQ(fl.n, 1);
    T_CHECK(last_tcp(&fl, &v)); T_EQ(v.flags, TF_ACK); T_EQ(v.seq, ms); T_EQ(v.ack, ps);
    T_EQ(mt_get_stats(s)->tcp_resets_in, 0);
    inject(s, 23, in.lport, ps + 1, 0, TF_RST, NULL, 0, 11);        /* one past: still no */
    T_EQ(r.n, 1); T_EQ(fl.n, 2);
    inject(s, 23, in.lport, ps, 0, TF_RST, NULL, 0, 12);            /* exact */
    T_EQ(r.n, 2); T_EQ(r.e[1].ev, MT_TCP_EV_TERMINATE); T_EQ(r.e[1].arg, MT_TERM_REMOTE_ABORT);
    T_EQ(fl.n, 2);
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_CLOSED);
    T_EQ(mt_get_stats(s)->tcp_resets_in, 1);
}

/* Advance to the next timer deadline; returns the new time. */
static uint32_t step(mt_stack *s, uint32_t now) {
    uint32_t d = mt_next_deadline(s, now);
    if (d == MT_NO_DEADLINE) return now;
    now += d;
    mt_tick(s, now);
    return now;
}

static int count_flags(const fake_link *fl, uint8_t flags) {
    int i, c = 0;
    tcpv v;
    for (i = 0; i < fl->n; i++)
        if (view_tcp(fl->f[i], fl->len[i], &v) && v.flags == flags) c++;
    return c;
}

static void test_synrcvd_gives_up_after_4_tries_and_relistens(void) {
    fake_link fl; rec r; mt_tcp_info in;
    mt_stack *s = setup(&fl);
    static uint8_t b[4096];
    mt_tcp *t = mt_tcp_create(s, b, sizeof b, rec_cb, &r);
    uint32_t now = 0;
    int i;
    memset(&r, 0, sizeof r);
    T_EQ(mt_tcp_listen(t, 0, 0, 21), MT_OK);
    inject(s, 2000, 21, 5, 0, TF_SYN, NULL, 0, 0);
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_SYN_RCVD);
    T_EQ(count_flags(&fl, TF_SYN | TF_ACK), 1);
    for (i = 0; i < 3; i++) {                                       /* +1 s, +3 s, +7 s */
        now = step(s, now);
        mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_SYN_RCVD);
    }
    T_EQ(now, 7000);
    T_EQ(count_flags(&fl, TF_SYN | TF_ACK), 4);
    now = step(s, now);                                             /* the 4th try times out */
    T_EQ(now, 15000);
    mt_tcp_get_info(t, &in);
    T_EQ(in.state, MT_TCP_LISTEN); T_EQ(in.lport, 21); T_EQ(in.rip, 0); T_EQ(in.rport, 0);
    T_EQ(count_flags(&fl, TF_SYN | TF_ACK), 4);                     /* no fifth */
    T_EQ(r.n, 0);                                                   /* the app never saw it */
    T_CHECK(mt_next_deadline(s, now) > 500000u);                    /* only the ARP entry's expiry left */
    inject(s, 2001, 21, 50, 0, TF_SYN, NULL, 0, now + 1);          /* same listener works again */
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_SYN_RCVD);
    T_EQ(count_flags(&fl, TF_SYN | TF_ACK), 5);
}

static void test_syn_sent_keeps_connect_timeout(void) {
    fake_link fl; rec r; mt_tcp_info in;
    mt_stack *s = setup(&fl);
    static uint8_t b[4096];
    mt_tcp *t = mt_tcp_create(s, b, sizeof b, rec_cb, &r);
    uint32_t now = 0;
    int i;
    memset(&r, 0, sizeof r);
    T_EQ(mt_tcp_open(t, T_PEER, 23, 0, 0), MT_OK);
    for (i = 0; i < 5; i++) now = step(s, now);   /* 1 3 7 15 31 s: inside the 120 s connect timeout */
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_SYN_SENT);
    T_CHECK(count_flags(&fl, TF_SYN) >= 6);                         /* well past four tries */
}

static void test_segment_to_port_0_dropped(void) {
    fake_link fl; mt_tcp_info in;
    mt_stack *s = setup(&fl);
    static uint8_t b[4096], ub[2048];
    mt_tcp *t = mt_tcp_create(s, b, sizeof b, NULL, NULL);
    mt_udp *u = mt_udp_create(s, 0, ub, sizeof ub, NULL, NULL);    /* gets an ephemeral port */
    const mt_stats *st = mt_get_stats(s);
    uint32_t d0;
    T_CHECK(u != NULL);
    T_EQ(mt_tcp_listen(t, 0, 0, 21), MT_OK);
    d0 = st->tcp_in_drops;
    inject(s, 1025, 0, 100, 0, TF_SYN, NULL, 0, 1);                 /* TCP to port 0 */
    inject(s, 1025, 0, 100, 0, TF_ACK, NULL, 0, 2);
    T_EQ(fl.n, 0);                                                  /* no RST */
    T_EQ(st->tcp_in_drops, d0 + 2);
    inject(s, 0, 21, 100, 0, TF_SYN, NULL, 0, 3);                   /* SYN from port 0 */
    T_EQ(fl.n, 0);
    T_EQ(st->tcp_in_drops, d0 + 3);
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_LISTEN);
    d0 = st->udp_drops;
    inject_udp(s, T_PEER, 5000, 0, 4);                              /* UDP to port 0 */
    T_EQ(fl.n, 0);                                                  /* no unreachable */
    T_EQ(st->udp_drops, d0 + 1);
    T_EQ(st->udp_noport, 0);
    T_EQ(mt_udp_queued(u), 0);                                      /* nor delivered to a free slot */
}

/* ---- ICMP ---- */

static void test_icmp_out_not_counted_on_send_failure(void) {
    fake_link fl;
    mt_stack *s = setup(&fl);
    const mt_stats *st = mt_get_stats(s);
    int (*tx)(void *, const uint8_t *, uint16_t);
    tx = s->link.tx;
    s->link.tx = NULL;                                              /* every transmit fails */
    inject_echo(s, T_PEER, 10);
    T_EQ(st->icmp_in, 1);
    T_EQ(st->icmp_out, 0);
    T_EQ(st->eth_tx_err, 1);
    T_EQ(mt_icmp_echo(s, T_PEER, 1, 1, NULL, 0, 11), MT_OK);
    T_EQ(st->icmp_out, 0);
    inject_udp(s, T_PEER, 5000, 9, 12);                             /* unreachable also fails */
    T_EQ(st->icmp_out, 0);
    s->link.tx = tx;
    inject_echo(s, T_PEER, 1000);
    T_EQ(fl.n, 1);
    T_EQ(st->icmp_out, 1);
    T_EQ(mt_icmp_echo(s, T_PEER, 1, 2, NULL, 0, 1001), MT_OK);
    T_EQ(st->icmp_out, 2);
}

static void test_unreach_rate_limited(void) {
    fake_link fl;
    mt_stack *s = setup(&fl);
    const mt_stats *st = mt_get_stats(s);
    inject_udp(s, T_PEER, 5000, 9, 1000);
    T_EQ(fl.n, 1);
    inject_udp(s, T_PEER, 5001, 9, 1050);
    inject_udp(s, T_PEER, 5002, 10, 1099);                          /* limit is per stack */
    T_EQ(fl.n, 1);
    T_EQ(st->icmp_out, 1);
    T_EQ(st->udp_noport, 3);                                        /* still counted as events */
    inject_udp(s, T_PEER, 5003, 9, 1100);
    T_EQ(fl.n, 2);
    T_EQ(st->icmp_out, 2);
    /* wrap-safe: near the clock wrap (the ARP entry is refreshed so frames are not held) */
    mt_tick(s, 0x70000000u);              /* ticks at least every 2^31 ms, as the contract requires */
    mt_tick(s, 0xE0000000u);
    mt_arp_insert(s, T_PEER, T_PEER_MAC, 0xFFFFFFF0u);
    inject_udp(s, T_PEER, 5004, 9, 0xFFFFFFF0u);
    T_EQ(fl.n, 3);
    inject_udp(s, T_PEER, 5005, 9, 0xFFFFFFF0u + 50u);
    T_EQ(fl.n, 3);
    inject_udp(s, T_PEER, 5006, 9, 0xFFFFFFF0u + 100u);             /* wrapped to 84 */
    T_EQ(fl.n, 4);
    /* a long idle gap in tick-sized strides is not stuck */
    mt_tick(s, 0xFFFFFFF0u + 100u + 0x70000000u);
    mt_tick(s, 0xFFFFFFF0u + 100u + 0xE0000000u);
    mt_arp_insert(s, T_PEER, T_PEER_MAC, 0xFFFFFFF0u + 100u + 0xE0000000u);
    inject_udp(s, T_PEER, 5007, 9, 0xFFFFFFF0u + 100u + 0xE0000000u);
    T_EQ(fl.n, 5);
}

/* The limiter must work whatever the clock starts at (zero-initialised state, upper half, wrap). */
static void unreach_from(uint32_t t0, int *two_apart, int *close_pair) {
    fake_link fl;
    mt_stack *s = setup(&fl);
    mt_arp_insert(s, T_PEER, T_PEER_MAC, t0);
    inject_udp(s, T_PEER, 5000, 9, t0);
    inject_udp(s, T_PEER, 5001, 9, t0 + 2000u);
    *two_apart = fl.n;
    fake_clear(&fl);
    mt_arp_insert(s, T_PEER, T_PEER_MAC, t0 + 10000u);
    inject_udp(s, T_PEER, 5002, 9, t0 + 10000u);
    inject_udp(s, T_PEER, 5003, 9, t0 + 10010u);
    *close_pair = fl.n;
}

static void test_unreach_limiter_any_clock_start(void) {
    static const uint32_t starts[] = {0u, 5u, 0x7FFFFFF0u, 0x80000000u, 0x90000000u, 0xFFFFFF00u,
                                      0xFFFFFFFFu};
    size_t i;
    for (i = 0; i < sizeof starts / sizeof starts[0]; i++) {
        int a, b;
        unreach_from(starts[i], &a, &b);
        T_EQ(a, 2);          /* 2 s apart: both answered (also across the 2^32 wrap) */
        T_EQ(b, 1);          /* 10 ms apart: one */
    }
}

static void test_rst_out_of_window_silent(void) {
    fake_link fl; rec r; mt_tcp_info in; uint32_t ps, ms;
    mt_stack *s = setup(&fl);
    mt_tcp *t;
    memset(&r, 0, sizeof r);
    t = tconn_established(s, &fl, &r, &ps, &ms);
    mt_tcp_get_info(t, &in);
    inject(s, 23, in.lport, ps + 100000u, 0, TF_RST, NULL, 0, 10);
    inject(s, 23, in.lport, ps - 5, 0, TF_RST, NULL, 0, 11);
    T_EQ(fl.n, 0);
    T_EQ(r.n, 1);
    mt_tcp_get_info(t, &in); T_EQ(in.state, MT_TCP_ESTABLISHED);
}

static void test_arp_merge_keeps_lru_stamp(void) {
    fake_link fl; uint8_t f[64], mac[6];
    mt_stack *s = setup(&fl);
    mt_arp_entry *e = NULL;
    uint16_t i, n;
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 100), 1);                   /* t_used = 100 */
    n = mk_arp(f, 1, MAC2, T_PEER, ZERO, T_GW);
    mt_input(s, f, n, 5000);
    for (i = 0; i < s->lim.arp_entries; i++)
        if (s->arp[i].state != MT_ARP_FREE && s->arp[i].ip == T_PEER) e = &s->arp[i];
    T_CHECK(e != NULL);
    if (!e) return;
    T_MEMEQ(e->mac, MAC2, 6);
    T_EQ(e->t_stamp, 5000);
    T_EQ(e->t_used, 100);
}

static void test_nonsyn_from_port_0_no_rst(void) {
    fake_link fl;
    mt_stack *s = setup(&fl);
    uint32_t d0 = mt_get_stats(s)->tcp_in_drops;
    inject(s, 0, 99, 100, 0, TF_ACK, NULL, 0, 1);                   /* closed port, source port 0 */
    inject(s, 0, 99, 100, 0, TF_FIN | TF_ACK, NULL, 0, 2);
    T_EQ(fl.n, 0);
    T_EQ(mt_get_stats(s)->tcp_in_drops, d0 + 2);
}

/* ---- delayed-ACK full-size test follows the largest received segment ---- */

static void test_rcv_mss_tracks_largest_segment(void) {
    fake_link fl; rec r; uint32_t ps, ms;
    static uint8_t data[1500];
    mt_stack *s = setup(&fl);
    mt_tcp *t;
    uint32_t now = 100;
    memset(&r, 0, sizeof r);
    memset(data, 0x41, sizeof data);
    t = tconn_established(s, &fl, &r, &ps, &ms);
    T_CHECK(t->rcv_mss <= t->mss);
    /* a peer behind MSS clamping sends 700-byte segments: ACK every second, not every one */
    inject(s, 23, t->lport, ps, ms, TF_ACK, data, 700, now);
    T_EQ(fl.n, 0);
    T_EQ(t->rcv_mss, 700);
    inject(s, 23, t->lport, ps + 700, ms, TF_ACK, data, 700, now);
    T_EQ(fl.n, 1);
    fake_clear(&fl);
    /* sub-size segments still draw an immediate ACK */
    inject(s, 23, t->lport, ps + 1400, ms, TF_ACK, data, 100, now);
    T_EQ(fl.n, 1);
    fake_clear(&fl);
    /* never above the local MSS, even if the peer overshoots it */
    inject(s, 23, t->lport, ps + 1500, ms, TF_ACK, data, 1480, now);
    T_EQ(t->rcv_mss, mt_tcp_local_mss(s));
    now += 100;
    mt_tick(s, now);                                                /* delayed ACK goes out */
    fake_clear(&fl);
    inject(s, 23, t->lport, ps + 2980, ms, TF_ACK, data, 1465, now);
    T_EQ(fl.n, 0);                                                  /* 1465 >= 1460: full-size */
}

T_MAIN(test_arp_merge_updates_existing_from_any_arp,
       test_arp_ignores_multicast_sender_and_own_mac,
       test_ip_rejects_multicast_own_and_subnet_broadcast_source,
       test_tcp_syn_from_multicast_no_rst, test_rst_exact_match_only,
       test_synrcvd_gives_up_after_4_tries_and_relistens, test_syn_sent_keeps_connect_timeout,
       test_segment_to_port_0_dropped, test_icmp_out_not_counted_on_send_failure,
       test_unreach_rate_limited, test_rcv_mss_tracks_largest_segment,
       test_unreach_limiter_any_clock_start, test_rst_out_of_window_silent,
       test_arp_merge_keeps_lru_stamp, test_nonsyn_from_port_0_no_rst)
