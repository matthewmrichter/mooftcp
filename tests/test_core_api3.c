#include "t.h"
#include "net.h"
#include "mt_int.h"
#include "mt_util.h"

#include "dnsb.h"

/* Plan 2 Task 4: mt_set_config semantics with live state, stale ULP deadline. */

static void test_set_config_addr_change_flushes_arp(void) {
    fake_link fl; mt_stack *s = tstack_new(&fl, T_ME);
    mt_ifconfig c;
    uint8_t mac[6];
    uint32_t drops;
    mt_arp_insert(s, T_PEER, T_PEER_MAC, 0);
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 1), 1);
    /* a frame held for an unresolved neighbour */
    T_EQ(mt_icmp_echo(s, T_PEER + 5, 1, 1, (const uint8_t *)"x", 1, 2), MT_OK);
    T_CHECK(s->pend->len != 0);
    drops = mt_get_stats(s)->arp_drops;
    mt_get_config(s, &c);
    c.addr = T_ME + 1;
    mt_set_config(s, &c);
    T_EQ(s->pend->len, 0);
    T_EQ(mt_get_stats(s)->arp_drops, drops + 1);            /* the discarded held frame is counted */
    fake_clear(&fl);
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 3), 0);             /* entry gone: ARP request again */
    T_EQ(fl.n, 1);
}

static void test_set_config_other_changes_keep_arp(void) {
    fake_link fl; mt_stack *s = tstack_new(&fl, T_ME);
    mt_ifconfig c;
    uint8_t mac[6];
    mt_arp_insert(s, T_PEER, T_PEER_MAC, 0);
    mt_get_config(s, &c);
    c.dns = T_DNS; strcpy(c.domain, "x.org"); c.mtu = 1400;     /* none of these touch routing */
    mt_set_config(s, &c);
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 1), 1);
    c.gateway = T_GW + 1;                                       /* gateway: flush */
    mt_set_config(s, &c);
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 2), 0);
    mt_arp_insert(s, T_PEER, T_PEER_MAC, 3);
    c.mask = 0xFFFF0000u;                                       /* mask: flush */
    mt_set_config(s, &c);
    T_EQ(mt_arp_resolve(s, T_PEER, mac, 4), 0);
}

static void test_set_config_mtu_shrink_reclamps_mss(void) {
    fake_link fl; mt_stack *s = tstack_new(&fl, T_ME);
    rec r; uint32_t ps, ms;
    mt_tcp *t;
    mt_ifconfig c;
    static uint8_t d[3000];
    mt_tcp_info in;
    int i, big = 0;
    memset(&r, 0, sizeof r);
    t = tconn_established(s, &fl, &r, &ps, &ms);
    T_CHECK(t != NULL);
    T_EQ(t->mss, 1460);
    mt_get_config(s, &c);
    c.mtu = 576;
    mt_set_config(s, &c);
    T_EQ(t->mss, 536);
    mt_tcp_get_info(t, &in);
    T_EQ(in.state, MT_TCP_ESTABLISHED);                      /* not reset */
    T_EQ(r.n, 1);                                            /* only the ESTABLISHED event */
    T_EQ(fl.n, 0);
    c.mtu = 1500;                                            /* growing never raises it */
    mt_set_config(s, &c);
    T_EQ(t->mss, 536);
    c.mtu = 296;                                             /* local MSS 256 */
    mt_set_config(s, &c);
    T_EQ(t->mss, 256);
    c.mtu = 576;
    mt_set_config(s, &c);
    T_EQ(t->mss, 256);
    /* segments really are built with the clamped MSS */
    memset(d, 7, sizeof d);
    T_EQ(tsend(t, d, 3000, 1, 0, 10), 3000);
    T_CHECK(fl.n >= 1);
    for (i = 0; i < fl.n; i++) {
        tcpv v;
        T_CHECK(view_tcp(fl.f[i], fl.len[i], &v));
        if (v.dlen > 256) big = 1;
    }
    T_EQ(big, 0);
}

static void test_set_config_dns_change_next_retry_uses_new_server(void) {
    fake_link fl; mt_stack *s = setup(&fl, "old.org");
    mt_dns_result r; got g = {0};
    mt_ifconfig c;
    qv q0, q1;
    pkt p;
    mt_ip4 ns = T_DNS + 1;
    T_EQ(mt_dns_query(s, "host", MT_DNS_Q_A, &r, on_done, &g, 0), MT_DNS_IN_PROGRESS);
    get_q(&fl, 0, &q0);
    T_EQ(q0.dst, T_DNS); T_CHECK(strcmp(q0.name, "host.old.org") == 0);
    mt_arp_insert(s, ns, T_PEER_MAC, 0);
    mt_get_config(s, &c);
    c.dns = ns;
    memset(c.domain, 0, sizeof c.domain); strcpy(c.domain, "new.org");
    mt_set_config(s, &c);
    T_EQ(g.n, 0);                                            /* nothing cancelled */
    T_EQ(mt_dns_deadline(s, 0), 2000);
    mt_tick(s, 2000);
    T_EQ(fl.n, 2);
    get_q(&fl, 1, &q1);
    T_EQ(q1.dst, ns);
    T_CHECK(strcmp(q1.name, "host.new.org") == 0);
    T_EQ(q1.id, q0.id);                                      /* same query, retried */
    /* an answer from the old server is ignored, from the new one accepted */
    begin(&p, q1.id, 0x8180, "host.new.org", 1);
    count_an(&p, 1);
    rr_a(&p, 300, 0x0A000007u);
    deliver(s, &q1, T_DNS, 53, &p, 2100);
    T_EQ(g.n, 0);
    deliver(s, &q1, ns, 53, &p, 2200);
    T_EQ(g.n, 1); T_EQ(g.r.status, MT_DNS_OK); T_EQ(g.r.addr[0], 0x0A000007u);
}

static void test_ulp_deadline_stale_when_ulp_set_later(void) {
    fake_link fl; mt_stack *s = tstack_new(&fl, T_ME);
    rec r; uint32_t ps, ms, now = 100;
    mt_tcp *t;
    static uint8_t d[100];
    memset(&r, 0, sizeof r);
    t = tconn_established(s, &fl, &r, &ps, &ms);
    T_CHECK(t != NULL);
    mt_tcp_set_ulp_timeout(t, 0, 1);                         /* no ULP timeout while sending */
    T_EQ(tsend(t, d, 100, 1, 0, now), 100);                  /* RTO armed, ulp_dl = now + 0 */
    r.n = 0;
    mt_tcp_set_ulp_timeout(t, 5, 1);                         /* set later: 5 s counted from the send */
    for (;;) {                                               /* peer silent: RTO retransmits */
        uint32_t w = mt_next_deadline(s, now);
        if (w == MT_NO_DEADLINE || now > 20000) break;
        now += w;
        mt_tick(s, now);
        if (r.n) break;
        T_CHECK(now < 100 + 5000);                           /* no ULP event before its time */
    }
    T_EQ(now, 100 + 5000);                                   /* exactly 5 s after the send */
    T_EQ(r.n, 2);
    T_EQ(r.e[0].ev, MT_TCP_EV_ULP_TIMEOUT);
    T_EQ(r.e[1].ev, MT_TCP_EV_TERMINATE);
}

static void test_ulp_change_moves_deadline_with_start(void) {
    fake_link fl; mt_stack *s = tstack_new(&fl, T_ME);
    rec r; uint32_t ps, ms;
    mt_tcp *t;
    static uint8_t d[100];
    memset(&r, 0, sizeof r);
    t = tconn_established(s, &fl, &r, &ps, &ms);
    mt_tcp_set_ulp_timeout(t, 30, 1);
    T_EQ(tsend(t, d, 100, 1, 0, 0), 100);
    r.n = 0;
    mt_tcp_set_ulp_timeout(t, 60, 1);                        /* deadline is start + 60 s */
    mt_tick(s, 30000);
    T_EQ(r.n, 0);
    mt_tick(s, 59999);
    T_EQ(r.n, 0);
    mt_tick(s, 60000);
    T_EQ(r.n, 2);
    /* shortening below the elapsed time fires at once */
    s = tstack_new(&fl, T_ME);
    memset(&r, 0, sizeof r);
    t = tconn_established(s, &fl, &r, &ps, &ms);
    mt_tcp_set_ulp_timeout(t, 30, 1);
    T_EQ(tsend(t, d, 100, 1, 0, 0), 100);
    r.n = 0;
    mt_tick(s, 3000);
    mt_tcp_set_ulp_timeout(t, 2, 1);
    T_EQ(mt_next_deadline(s, 3000), 0);
    mt_tick(s, 3000);
    T_EQ(r.n, 2);
}

/* Plan 2 Task 7: mt_tcp_info carries what TCPStatus reports; once our FIN is ACKed nothing is
 * unacknowledged (the FIN's sequence number used to make `unacked` wrap to 0xFFFFFFFF). */
static void test_info_after_fin_acked(void) {
    fake_link fl; mt_stack *s = tstack_new(&fl, T_ME);
    rec r; uint32_t ps, ms; uint8_t f[200]; uint16_t n;
    mt_tcp_info in;
    mt_tcp *t;
    memset(&r, 0, sizeof r);
    t = tconn_established(s, &fl, &r, &ps, &ms);
    T_CHECK(t != NULL);
    if (!t) return;
    mt_tcp_get_info(t, &in);
    T_EQ(in.snd_una, ms);
    T_EQ(in.snd_nxt, ms);
    T_EQ(in.rcv_nxt, ps);
    T_EQ(in.mss, 1460);
    T_CHECK(in.cwnd >= 1460);
    T_EQ(mt_tcp_close(t, 1), MT_OK);
    mt_tcp_get_info(t, &in);
    T_EQ(in.state, MT_TCP_FIN_WAIT_1);
    T_EQ(in.unacked, 0);                                    /* only the FIN is outstanding */
    n = mk_tcp(f, T_PEER, 23, T_ME, t->lport, ps, ms + 1, TF_ACK, 8192, NULL, 0, NULL, 0);
    mt_input(s, f, n, 2);
    mt_tcp_get_info(t, &in);
    T_EQ(in.state, MT_TCP_FIN_WAIT_2);
    T_EQ(in.unacked, 0);
    T_EQ(in.send_queued, 0);
}

T_MAIN(test_set_config_addr_change_flushes_arp, test_set_config_other_changes_keep_arp,
       test_set_config_mtu_shrink_reclamps_mss,
       test_set_config_dns_change_next_retry_uses_new_server,
       test_ulp_deadline_stale_when_ulp_set_later, test_ulp_change_moves_deadline_with_start,
       test_info_after_fin_acked)
