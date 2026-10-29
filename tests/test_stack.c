#include "t.h"
#include "net.h"
#include "pair.h"
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

/* ---- statistics and diagnostic log ---- */

static void pair_cb(mt_tcp *t, mt_tcp_event ev, uint32_t arg, void *user) {
    static uint8_t sink[4096];
    uint32_t now = pair_now((pair *)user);
    (void)arg;
    if (ev == MT_TCP_EV_DATA) while (mt_tcp_recv(t, sink, sizeof sink, now)) {}
    else if (ev == MT_TCP_EV_CLOSING) (void)mt_tcp_close(t, now);
}

static void test_stats_counts(void) {
    pair *p = pair_new(&(pair_faults){0, 0, 0, 0, 1, 1}, 0);
    mt_stack *a = pair_a(p), *b = pair_b(p);
    static uint8_t ba[8192], bb[8192];
    mt_tcp *ta = mt_tcp_create(a, ba, sizeof ba, pair_cb, p);
    mt_tcp *tb = mt_tcp_create(b, bb, sizeof bb, pair_cb, p);
    T_EQ(mt_tcp_listen(tb, 0, 0, 7000), MT_OK);
    T_EQ(mt_tcp_open(ta, PAIR_B_IP, 7000, 0, pair_now(p)), MT_OK);   /* triggers one ARP exchange */
    pair_run(p, 200);
    T_EQ(mt_get_stats(a)->arp_out, 1);
    T_EQ(mt_get_stats(a)->arp_in, 1);
    T_EQ(mt_get_stats(a)->tcp_active_opens, 1);
    T_EQ(mt_get_stats(b)->tcp_passive_opens, 1);
    T_EQ(mt_get_stats(a)->tcp_conns_open, 1);
    T_EQ(mt_tcp_count_open(a), 1);
    T_EQ(mt_tcp_close(ta, pair_now(p)), MT_OK);
    pair_run(p, 10000);                               /* FIN handshake, then TIME_WAIT expires */
    T_EQ(mt_get_stats(a)->tcp_conns_open, 0);
    T_EQ(mt_get_stats(b)->tcp_conns_open, 0);
    T_EQ(mt_tcp_count_open(a), 0);
    T_EQ(mt_tcp_count_open(b), 0);
    T_EQ(mt_get_stats(a)->tcp_active_opens, 1);
    mt_tcp_release(ta); mt_tcp_release(tb);
    pair_free(p);
}

static void log_activity(mt_stack *s, fake_link *fl) {
    uint8_t f[64];
    static const uint8_t zero[6] = {0};
    static uint8_t buf[4096];
    mt_tcp *t = mt_tcp_create(s, buf, sizeof buf, NULL, NULL);
    uint16_t n = mk_arp(f, 1, T_PEER_MAC, T_PEER, zero, T_ME);
    mt_input(s, f, n, 1);
    n = mk_ip(f, T_PEER, T_ME, 17, 1, 0, (const uint8_t *)"ab", 2);   /* short UDP: a drop */
    mt_input(s, f, n, 2);
    T_EQ(mt_tcp_listen(t, 0, 0, 80), MT_OK);
    T_EQ(mt_tcp_close(t, 3), MT_OK);
    mt_tcp_release(t);
    (void)fl;
}

static void test_log_disabled_by_default(void) {
    fake_link fl; mt_logrec r[4];
    mt_stack *s = tstack_new(&fl, T_ME);
    log_activity(s, &fl);
    T_EQ(mt_log_read(s, r, 4), 0);
}

static void test_log_records_and_wraps(void) {
    fake_link fl; mt_logrec r[MT_LOG_RECS + 8];
    static uint8_t buf[4096];
    mt_stack *s = tstack_new(&fl, T_ME);
    mt_tcp *t = mt_tcp_create(s, buf, sizeof buf, NULL, NULL);
    uint16_t n, i;
    uint32_t last = 0;
    mt_log_enable(s, 1);
    log_activity(s, &fl);                              /* a drop and state changes are recorded */
    n = mt_log_read(s, r, 3);
    T_EQ(n, 3);                                        /* oldest first, partial drain */
    T_EQ(r[0].code, MT_LOG_DROP);
    T_EQ(r[0].a, MT_LOGL_UDP);
    T_EQ(r[0].t, 2);
    T_EQ(r[1].code, MT_LOG_TCP_STATE);
    T_EQ(r[1].a, MT_TCP_LISTEN);
    T_EQ(r[1].b, 80);
    T_EQ(r[2].a, MT_TCP_CLOSED);
    while (mt_log_read(s, r, 3)) {}                    /* drained: empty now */
    T_EQ(mt_log_read(s, r, 3), 0);
    for (i = 0; i < 100; i++) {                        /* 200 state changes */
        mt_tick(s, 100u + i);
        T_EQ(mt_tcp_listen(t, 0, 0, 1000u + i), MT_OK);
        T_EQ(mt_tcp_close(t, 100u + i), MT_OK);
    }
    n = mt_log_read(s, r, MT_LOG_RECS + 8);
    T_EQ(n, MT_LOG_RECS - 1);                          /* newest 127: one slot is the writer's */
    for (i = 0; i < n; i++) {
        T_CHECK(r[i].t >= last);
        last = r[i].t;
    }
    T_EQ(r[n - 1].a, MT_TCP_CLOSED);
    T_EQ(r[n - 1].t, 199);                             /* newest kept */
    T_EQ(r[0].t, 136);                                 /* oldest 73 were dropped */
    T_EQ(r[0].a, MT_TCP_CLOSED);
    T_EQ(r[0].b, 1036);
    mt_log_enable(s, 0);
    mt_tick(s, 500);
    T_EQ(mt_tcp_listen(t, 0, 0, 5), MT_OK);
    T_EQ(mt_log_read(s, r, 4), 0);                     /* off again: nothing recorded */
    mt_tcp_release(t);
}

/* Self-checking records: t = seq, a = low byte of seq, b = ~seq. */
static void put_seq(mt_stack *s, uint32_t seq) {
    s->now = seq;
    mt_log_put(s, MT_LOG_DROP, (uint8_t)seq, (uint16_t)~seq);
}
static int rec_ok(const mt_logrec *r) {
    return r->code == MT_LOG_DROP && r->a == (uint8_t)r->t && r->b == (uint16_t)~r->t;
}
static mt_stack *race_stack;
static mt_logrec race_buf[MT_LOG_RECS];
static uint16_t race_n;
static void race_spy(mt_stack *s) {            /* a reader interrupts the writer mid-record */
    (void)s;
    race_n = mt_log_read(race_stack, race_buf, MT_LOG_RECS);
}

static void test_log_no_torn_records(void) {
    fake_link fl; mt_logrec r[MT_LOG_RECS + 8];
    static const uint16_t pending[] = {127, 128, 129, 255, 300};
    uint16_t k, i, n;
    mt_stack *s = tstack_new(&fl, T_ME);
    mt_log_enable(s, 1);
    for (k = 0; k < sizeof pending / sizeof pending[0]; k++) {   /* plain: never torn, newest kept */
        uint32_t seq0 = 1000u * (k + 1u), last;
        for (i = 0; i < pending[k]; i++) put_seq(s, seq0 + i);
        n = mt_log_read(s, r, MT_LOG_RECS + 8);
        T_CHECK(n >= 1 && n <= MT_LOG_RECS - 1);
        for (i = 0; i < n; i++) T_CHECK(rec_ok(&r[i]));
        last = seq0 + pending[k] - 1u;
        T_EQ(r[n - 1].t, last);
        for (i = 1; i < n; i++) T_EQ(r[i].t, r[i - 1].t + 1);   /* contiguous, oldest first */
    }
    /* Deterministic race: ring holds 128 unread records, the writer is mid-record in the very
     * slot the oldest record occupies when a reader runs. That slot must not be returned. */
    {
        mt_stack *s2 = tstack_new(&fl, T_ME);
        uint32_t seq;
        mt_log_enable(s2, 1);
        for (seq = 5000; seq < 5000 + MT_LOG_RECS; seq++) put_seq(s2, seq);
        race_stack = s2;
        race_n = 0;
        s2->log_put_spy = race_spy;
        put_seq(s2, 5000 + MT_LOG_RECS);
        s2->log_put_spy = NULL;
        T_CHECK(race_n >= 1);
        for (i = 0; i < race_n; i++) T_CHECK(rec_ok(&race_buf[i]));
        for (i = 1; i < race_n; i++) T_EQ(race_buf[i].t, race_buf[i - 1].t + 1);
        n = mt_log_read(s2, r, MT_LOG_RECS + 8);                /* and the rest is consistent too */
        for (i = 0; i < n; i++) T_CHECK(rec_ok(&r[i]));
    }
}

/* ---- every counter is bumped by some scripted scenario ---- */

static const char *const stat_names[] = {
    "eth_in", "eth_out", "eth_tx_err", "arp_in", "arp_out", "arp_drops",
    "ip_in", "ip_out", "ip_in_drops", "ip_frag_out", "ip_reasm_ok", "ip_reasm_fail",
    "icmp_in", "icmp_out", "udp_in", "udp_out", "udp_drops", "udp_noport",
    "tcp_in", "tcp_out", "tcp_in_drops", "tcp_rexmt", "tcp_fast_rexmt", "tcp_active_opens",
    "tcp_passive_opens", "tcp_resets_in", "tcp_resets_out", "tcp_conns_open"
};
#define N_STATS (sizeof stat_names / sizeof stat_names[0])
/* C99 compile-time check: the name table covers exactly the (all-uint32) fields of mt_stats. */
typedef char stat_names_cover_struct[(sizeof(mt_stats) == N_STATS * sizeof(uint32_t)) ? 1 : -1];
#define STAT_GAUGE (N_STATS - 1)   /* tcp_conns_open: a gauge, so it is checked (back to 0) instead */

static void stats_add(uint32_t *sum, const mt_stack *s) {
    uint32_t v[N_STATS];
    size_t i;
    memcpy(v, mt_get_stats(s), sizeof v);
    for (i = 0; i < N_STATS; i++) sum[i] += v[i];
}

static int drop_nth_data(void *user, int from, const uint8_t *f, uint16_t len) {
    tcpv v;
    int *cnt = (int *)user;
    if (from != 0 || !view_tcp(f, len, &v) || v.dlen == 0) return 0;
    return ++*cnt == 12;                               /* lose one segment of a long flight */
}

static void test_stats_all_counters_live(void) {
    uint32_t sum[N_STATS];
    mt_stats sa, sb, ss;      /* per stack: A, B (pair) and S (single stack on a fake wire) */
    size_t i;
    static uint8_t big[60000], pay[3000], ba[65536], bb[32768], ub[8192], ub2[2048], trb[4096];
    int cnt = 0;
    uint32_t sent = 0;
    memset(sum, 0, sizeof sum);
    memset(pay, 0x5A, sizeof pay);
    {   /* two real stacks: ICMP, ARP, fragmented UDP, a TCP bulk transfer with one loss, a refused connect */
        pair *p = pair_new(&(pair_faults){0, 0, 0, 0, 5, 1}, 0);
        mt_stack *a = pair_a(p), *b = pair_b(p);
        mt_tcp *ta = mt_tcp_create(a, ba, sizeof ba, pair_cb, p);
        mt_tcp *tb = mt_tcp_create(b, bb, sizeof bb, pair_cb, p);
        mt_tcp *tr = mt_tcp_create(a, trb, sizeof trb, pair_cb, p);   /* for the refused connect */
        mt_udp *ua, *ub_;
        int step;
        T_EQ(mt_icmp_echo(a, PAIR_B_IP, 1, 1, pay, 8, pair_now(p)), MT_OK);
        pair_run(p, 100);
        ua = mt_udp_create(a, 0, ub2, sizeof ub2, NULL, NULL);
        ub_ = mt_udp_create(b, 9000, ub, sizeof ub, NULL, NULL);
        T_CHECK(ua && ub_);
        T_EQ(mt_udp_send(ua, PAIR_B_IP, 9000, pay, sizeof pay, 1, pair_now(p)), MT_OK);   /* fragments */
        T_EQ(mt_udp_send(ua, PAIR_B_IP, 9001, pay, 10, 1, pair_now(p)), MT_OK);           /* no port */
        pair_run(p, 100);
        T_EQ(mt_tcp_open(tr, PAIR_B_IP, 7999, 0, pair_now(p)), MT_OK);                   /* RST */
        pair_run(p, 100);
        memset(big, 0x33, sizeof big);
        pair_set_filter(p, drop_nth_data, &cnt);
        T_EQ(mt_tcp_listen(tb, 0, 0, 7000), MT_OK);
        T_EQ(mt_tcp_open(ta, PAIR_B_IP, 7000, 0, pair_now(p)), MT_OK);
        for (step = 0; step < 4000 && sent < sizeof big; step++) {
            int32_t r = mt_tcp_send(ta, big + sent, sizeof big - sent, 0, 0, pair_now(p));
            if (r > 0) sent += (uint32_t)r;
            pair_run(p, 5);
        }
        T_EQ(sent, sizeof big);
        T_EQ(mt_tcp_close(ta, pair_now(p)), MT_OK);
        pair_run(p, 20000);
        T_EQ(mt_get_stats(a)->tcp_conns_open, 0);
        T_EQ(mt_get_stats(b)->tcp_conns_open, 0);
        sa = *mt_get_stats(a); sb = *mt_get_stats(b);
        stats_add(sum, a); stats_add(sum, b);
        mt_tcp_release(ta); mt_tcp_release(tb); mt_tcp_release(tr);
        mt_udp_release(ua); mt_udp_release(ub_);
        pair_free(p);
    }
    {   /* one stack on a fake wire: malformed input, reassembly timeout, ARP give-up, tx errors */
        fake_link fl; uint8_t f[128];
        static const uint8_t zero[6] = {0};
        mt_stack *s = tstack_new(&fl, T_ME);
        uint16_t n, k;
        mt_logrec lr[MT_LOG_RECS];
        int seen[8] = {0};
        mt_ifconfig c;
        mt_dns_result dr;
        mt_log_enable(s, 1);
        n = mk_ip(f, T_PEER, T_ME, 17, 1, 0, (const uint8_t *)"abcd", 4);
        f[14 + 8] ^= 0xFF;                                         /* bad IP header checksum */
        mt_input(s, f, n, 0);
        n = mk_ip(f, T_PEER, T_ME, 17, 1, 0, (const uint8_t *)"abcd", 4);   /* UDP shorter than header */
        mt_input(s, f, n, 0);
        n = mk_tcp(f, T_PEER, 5000, T_ME, 80, 1, 0, TF_SYN, 1000, NULL, 0, NULL, 0);
        f[n - 1] ^= 0x55;                                          /* bad TCP checksum */
        mt_input(s, f, n, 0);
        n = mk_ip(f, T_PEER, T_ME, 17, 7, 0x2000, pay, 16);        /* first fragment only */
        mt_input(s, f, n, 0);
        T_EQ(mt_icmp_echo(s, T_FAR + 0, 1, 1, pay, 4, 0), MT_OK);  /* gateway never answers ARP */
        mt_tick(s, 10000);
        mt_get_config(s, &c);
        c.dns = T_PEER;
        mt_set_config(s, &c);
        T_EQ(mt_dns_query(s, "example.com", MT_DNS_Q_A, &dr, NULL, NULL, 10000), MT_DNS_IN_PROGRESS);
        mt_tick(s, 100000);                                        /* DNS gives up */
        T_EQ(dr.status, MT_DNS_TIMEOUT);
        n = mk_arp(f, 1, T_PEER_MAC, T_PEER, zero, T_ME);
        for (i = 0; i < 70; i++) mt_input(s, f, n, 100000);        /* the wire refuses after 64 */
        ss = *mt_get_stats(s);
        stats_add(sum, s);
        k = mt_log_read(s, lr, MT_LOG_RECS);
        for (n = 0; n < k; n++) {
            if (lr[n].code == MT_LOG_DROP && lr[n].a < 6) seen[lr[n].a] = 1;
            else if (lr[n].code == MT_LOG_ARP_FAIL) seen[6] = 1;
            else if (lr[n].code == MT_LOG_DNS_FAIL && lr[n].a == MT_DNS_TIMEOUT) seen[7] = 1;
        }
        for (n = 1; n < 8; n++)                                    /* drops on ETH..TCP, ARP and DNS failures */
            if (n != MT_LOGL_UDP + 0 || 1) T_CHECK(seen[n]);
    }
    /* Exact values, per stack: catches counters bumped on the wrong stack or twice. */
    T_EQ(sa.arp_out, 1); T_EQ(sa.arp_in, 1); T_EQ(sb.arp_out, 1); T_EQ(sb.arp_in, 1);
    T_EQ(sa.tcp_active_opens, 2);        /* the refused connect and the bulk connection */
    T_EQ(sa.tcp_passive_opens, 0); T_EQ(sb.tcp_active_opens, 0);
    T_EQ(sb.tcp_passive_opens, 1);       /* only the bulk connection had a listener */
    T_EQ(sb.tcp_resets_out, 1); T_EQ(sa.tcp_resets_in, 1);   /* the refused connect */
    T_EQ(sa.tcp_resets_out, 0); T_EQ(sb.tcp_resets_in, 0);
    T_EQ(sb.ip_reasm_ok, 1); T_EQ(sa.ip_reasm_ok, 0);        /* the 3000-byte UDP datagram */
    T_EQ(sa.ip_frag_out, 3); T_EQ(sb.ip_frag_out, 0);
    T_EQ(sb.udp_noport, 1); T_EQ(sa.udp_noport, 0);          /* one datagram to a closed port */
    T_EQ(sb.udp_in, 1);
    T_EQ(sa.icmp_out, 1); T_EQ(sb.icmp_in, 1);               /* echo request */
    T_EQ(sb.icmp_out, 2); T_EQ(sa.icmp_in, 2);               /* echo reply + port unreachable */
    T_CHECK(sa.tcp_fast_rexmt >= 1);
    T_CHECK(sa.tcp_rexmt >= sa.tcp_fast_rexmt);
    T_EQ(sb.tcp_fast_rexmt, 0);
    T_EQ(ss.tcp_in_drops, 1);            /* the one corrupted segment */
    T_EQ(ss.ip_in_drops, 1);             /* the one corrupted IP header */
    T_EQ(ss.udp_drops, 1); T_EQ(ss.ip_reasm_fail, 1); T_EQ(ss.ip_reasm_ok, 0);
    T_EQ(ss.arp_in, 70);                 /* 70 ARP requests injected */
    T_EQ(ss.arp_out, 64);                /* ARP frames the wire accepted (it takes 64 in all)... */
    T_EQ(ss.eth_tx_err, 8);              /* ...and those it refused after 64 frames */
    for (i = 0; i < N_STATS; i++) T_CHECK(stat_names[i] != NULL);
    for (i = 0; i < N_STATS; i++)
        if (i != STAT_GAUGE && sum[i] == 0) {
            fprintf(stderr, "%s:%d: counter %s was never bumped\n", __FILE__, __LINE__, stat_names[i]);
            t_failed = 1;
        }
}

T_MAIN(test_stack_size_budget, test_unconfigured_silent, test_demux_filters, test_tx_error_counted,
       test_stats_counts, test_log_disabled_by_default, test_log_records_and_wraps,
       test_log_no_torn_records, test_stats_all_counters_live)
