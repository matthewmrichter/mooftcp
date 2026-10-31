/* Plan 3 Task 3: loopback to our own address (cfg.addr and 127.0.0.0/8). MacTCP loops a TCP
 * connection to its own address back (matrix 703) and MacTCP Watcher's UDP/TCP self-tests use
 * it. Datagrams to ourselves never reach link.tx; they wait in the loopback FIFO until mt_tick
 * (mt_next_deadline is 0 meanwhile), one hop per tick. */
#include "t.h"
#include "net.h"
#include "mt_int.h"
#include "mt_util.h"

#define LO1 0x7F000001u   /* 127.0.0.1 */

typedef struct { int n; mt_ip4 from; uint16_t id, seq, len; } ecb;
static void echo_cb(void *u, mt_ip4 from, uint16_t id, uint16_t seq, const uint8_t *d,
                    uint16_t len, uint32_t now) {
    ecb *e = (ecb *)u;
    (void)d; (void)now;
    e->n++; e->from = from; e->id = id; e->seq = seq; e->len = len;
}

/* Run the stack as an embedder does: tick whenever the deadline is due, until `until`.
 * Returns the number of ticks. Bounded: a stack that keeps asking for 0 fails the test. */
static int run(mt_stack *s, uint32_t *now, uint32_t until) {
    int ticks = 0, zero = 0;
    for (;;) {
        uint32_t d = mt_next_deadline(s, *now);
        if (d == 0) {
            mt_tick(s, *now);
            ticks++;
            if (++zero > 10000) { T_CHECK(!"deadline stuck at 0"); return ticks; }
            continue;
        }
        zero = 0;
        if (d == MT_NO_DEADLINE || *now + d > until) { *now = until; return ticks; }
        *now += d;
    }
}

/* ---- ICMP ---- */

static void test_loopback_ping(void) {
    fake_link fl; mt_stack *s = tstack_new(&fl, T_ME);
    ecb e; uint32_t now = 0;
    const mt_stats *st = mt_get_stats(s);
    memset(&e, 0, sizeof e);
    mt_set_echo_cb(s, echo_cb, &e);
    T_EQ(mt_icmp_echo(s, T_ME, 7, 1, (const uint8_t *)"selftest", 8, now), MT_OK);
    T_EQ(fl.n, 0);                                   /* never on the wire, no ARP for ourselves */
    T_EQ(e.n, 0);                                    /* not delivered from inside the call */
    T_EQ(mt_next_deadline(s, now), 0);
    mt_tick(s, now);                                 /* request delivered, reply queued */
    T_EQ(e.n, 0);
    T_EQ(mt_next_deadline(s, now), 0);
    mt_tick(s, now);                                 /* reply delivered */
    T_EQ(e.n, 1); T_EQ(e.from, T_ME); T_EQ(e.id, 7); T_EQ(e.seq, 1); T_EQ(e.len, 8);
    T_EQ(mt_next_deadline(s, now), MT_NO_DEADLINE);
    T_EQ(st->icmp_out, 2); T_EQ(st->icmp_in, 2); T_EQ(st->ip_lo_out, 2); T_EQ(st->eth_out, 0);
    /* 127.0.0.1: the reply comes from 127.0.0.1 */
    T_EQ(mt_icmp_echo(s, LO1, 7, 2, (const uint8_t *)"x", 1, now), MT_OK);
    run(s, &now, 10);
    T_EQ(e.n, 2); T_EQ(e.from, LO1); T_EQ(e.seq, 2);
    T_EQ(fl.n, 0);
}

/* ---- UDP ---- */

typedef struct { int n; mt_ip4 src; uint16_t sport, len; uint8_t data[2048]; int reply; } ucb;
static void udp_cb(mt_udp *u, mt_udp_event ev, uint32_t arg, void *user) {
    ucb *c = (ucb *)user;
    mt_udp_dgram dg;
    (void)arg;
    if (ev != MT_UDP_EV_DATA) return;
    while (mt_udp_peek(u, &dg)) {
        c->n++; c->src = dg.src; c->sport = dg.sport; c->len = dg.len;
        if (dg.len <= sizeof c->data) memcpy(c->data, dg.data, dg.len);
        if (c->reply)                                /* echo server: back to the sender */
            mt_udp_send(u, dg.src, dg.sport, dg.data, dg.len, 1, 0);
        mt_udp_done(u);
    }
}

static void test_loopback_udp_echo(void) {
    fake_link fl; mt_stack *s = tstack_new(&fl, T_ME);
    static uint8_t ba[4096], bb[4096], big[1700];
    ucb ca, cb; uint32_t now = 0, i;
    mt_udp *ua, *ub;
    const mt_stats *st = mt_get_stats(s);
    memset(&ca, 0, sizeof ca); memset(&cb, 0, sizeof cb);
    ca.reply = 1;
    ua = mt_udp_create(s, 7, ba, sizeof ba, udp_cb, &ca);      /* echo server on port 7 */
    ub = mt_udp_create(s, 0, bb, sizeof bb, udp_cb, &cb);
    T_CHECK(ua && ub);
    if (!ua || !ub) return;
    T_EQ(mt_udp_send(ub, T_ME, 7, (const uint8_t *)"ping", 4, 1, now), MT_OK);
    T_EQ(fl.n, 0);
    mt_tick(s, now);
    T_EQ(ca.n, 1); T_EQ(ca.src, T_ME); T_EQ(ca.sport, mt_udp_port(ub)); T_EQ(cb.n, 0);
    mt_tick(s, now);
    T_EQ(cb.n, 1); T_EQ(cb.src, T_ME); T_EQ(cb.sport, 7); T_EQ(cb.len, 4);
    T_MEMEQ(cb.data, "ping", 4);
    /* without a checksum, and to 127.0.0.1 (the source is 127.0.0.1 too) */
    T_EQ(mt_udp_send(ub, LO1, 7, (const uint8_t *)"abc", 3, 0, now), MT_OK);
    run(s, &now, 10);
    T_EQ(cb.n, 2); T_EQ(cb.src, LO1); T_EQ(ca.src, LO1);
    /* bigger than the loopback MTU: fragmented into the FIFO, reassembled, echoed (fragmented).
     * The echo is sent while the last request fragment is being delivered (its slot is still
     * reserved), so a reply can use MT_LO_SLOTS - 1 slots: 2 fragments here. */
    for (i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 7u + 3u);
    T_EQ(mt_udp_send(ub, T_ME, 7, big, 1000, 1, now), MT_OK);
    T_EQ(st->ip_frag_out, 2);
    run(s, &now, 20);
    T_EQ(cb.n, 3); T_EQ(cb.len, 1000); T_MEMEQ(cb.data, big, 1000);
    T_EQ(st->ip_reasm_ok, 2); T_EQ(st->ip_frag_out, 4);
    T_EQ(st->udp_drops, 0); T_EQ(st->ip_lo_drops, 0);
    /* three fragments go in (1200 bytes) but the 3-fragment echo cannot: it is sent while the
     * last request fragment holds its slot, 2 are free, so the whole reply is refused before any
     * fragment is queued (one ip_lo_drops) and the reassembly slot stays free */
    T_EQ(mt_udp_send(ub, T_ME, 7, big, 1200, 1, now), MT_OK);
    T_EQ(st->ip_frag_out, 7);
    run(s, &now, 30);
    T_EQ(ca.n, 4); T_EQ(ca.len, 1200); T_EQ(cb.n, 3); T_EQ(st->ip_lo_drops, 1);
    T_EQ(st->ip_frag_out, 7);                                 /* no reply fragment was built */
    T_EQ(s->reasm->active, 0); T_EQ(st->ip_reasm_fail, 0);
    /* so a fragmented datagram from the wire reassembles right away */
    {
        uint8_t f[100], u[24];
        uint16_t n;
        memset(u, 0, sizeof u);
        mt_wr16(u, 4000); mt_wr16(u + 2, mt_udp_port(ub)); mt_wr16(u + 4, 24);   /* checksum 0 */
        memcpy(u + 8, "wirefragmentsok!", 16);
        n = mk_ip(f, T_PEER, T_ME, 17, 99, 0x2000, u, 16);
        mt_input(s, f, n, now);
        n = mk_ip(f, T_PEER, T_ME, 17, 99, 2, u + 16, 8);
        mt_input(s, f, n, now);
        T_EQ(st->ip_reasm_ok, 4); T_EQ(cb.n, 4); T_EQ(cb.src, T_PEER); T_EQ(cb.len, 16);
        T_MEMEQ(cb.data, "wirefragmentsok!", 16);
    }
    /* the largest datagram that fits 3 fragments (UDP payload 1648) is accepted; one more byte
     * needs 4 fragments, which never fit: MT_ERR_INVAL, nothing queued, not a refusal */
    T_EQ(mt_udp_send(ub, T_ME, 4444, big, 1648, 1, now), MT_OK);
    T_EQ(st->ip_lo_out, 14);
    run(s, &now, 40);
    T_EQ(mt_udp_send(ub, T_ME, 7, big, 1649, 1, now), MT_ERR_INVAL);
    T_EQ(mt_udp_send(ub, T_ME, 7, big, 1700, 1, now), MT_ERR_INVAL);
    T_EQ(st->ip_lo_out, 14); T_EQ(st->ip_lo_drops, 1); T_EQ(st->ip_reasm_fail, 0);
    T_EQ(s->reasm->active, 0);
    /* a fragmented datagram with too few free slots: refused whole, the FIFO keeps what it had */
    T_EQ(mt_udp_send(ub, T_ME, 4444, (const uint8_t *)"x", 1, 1, now), MT_OK);
    T_EQ(mt_udp_send(ub, T_ME, 4444, big, 600, 1, now), MT_OK);     /* 2 fragments, 2 free: ok */
    T_EQ(mt_udp_send(ub, T_ME, 4444, big, 600, 1, now), MT_OK);     /* 0 free: refused */
    T_EQ(s->lo->n, 3); T_EQ(st->ip_lo_drops, 2);
    run(s, &now, 50);
    T_EQ(s->reasm->active, 0); T_EQ(st->ip_reasm_fail, 0);
    T_EQ(mt_next_deadline(s, now), MT_NO_DEADLINE);
    T_EQ(fl.n, 0); T_EQ(st->eth_out, 0);
    mt_udp_release(ua); mt_udp_release(ub);
}

/* ---- TCP ---- */

static void test_loopback_tcp_connect_send_close(void) {
    fake_link fl; mt_stack *s = tstack_new(&fl, T_ME);
    static uint8_t ra[8192], rb[8192], data[20000], got[20000];
    rec ea, eb; mt_tcp_info ia, ib;
    uint32_t now = 0, sent = 0, rcvd = 0, i;
    int step;
    mt_tcp *a, *b;
    const mt_stats *st = mt_get_stats(s);
    memset(&ea, 0, sizeof ea); memset(&eb, 0, sizeof eb);
    a = mt_tcp_create(s, ra, sizeof ra, rec_cb, &ea);
    b = mt_tcp_create(s, rb, sizeof rb, rec_cb, &eb);
    T_CHECK(a && b);
    if (!a || !b) return;
    T_EQ(mt_tcp_listen(a, 0, 0, 17779), MT_OK);
    T_EQ(mt_tcp_open(b, T_ME, 17779, 0, now), MT_OK);         /* SYN queued, not sent */
    T_EQ(fl.n, 0);
    T_EQ(mt_next_deadline(s, now), 0);
    /* One hop per tick (the shape of MacTCP's matrix 703: the active side is established while
     * the passive side is still in SYN-received). */
    mt_tick(s, now);                                          /* SYN -> a; SYN-ACK queued */
    mt_tcp_get_info(a, &ia); mt_tcp_get_info(b, &ib);
    T_EQ(ia.state, MT_TCP_SYN_RCVD); T_EQ(ib.state, MT_TCP_SYN_SENT);
    mt_tick(s, now);                                          /* SYN-ACK -> b; ACK queued */
    mt_tcp_get_info(a, &ia); mt_tcp_get_info(b, &ib);
    T_EQ(ia.state, MT_TCP_SYN_RCVD); T_EQ(ib.state, MT_TCP_ESTABLISHED);
    T_EQ(ia.unacked, 1);                                      /* its SYN (MacTCP 703: a.unack=1) */
    T_EQ(eb.n, 1); T_EQ(eb.e[0].ev, MT_TCP_EV_ESTABLISHED); T_EQ(ea.n, 0);
    mt_tick(s, now);                                          /* ACK -> a */
    mt_tcp_get_info(a, &ia); mt_tcp_get_info(b, &ib);
    T_EQ(ia.state, MT_TCP_ESTABLISHED);
    T_EQ(ea.n, 1); T_EQ(ea.e[0].ev, MT_TCP_EV_ESTABLISHED);
    T_EQ(ia.lip, T_ME); T_EQ(ia.rip, T_ME); T_EQ(ia.rport, ib.lport); T_EQ(ib.rport, 17779);
    T_EQ(ia.mss, MT_LO_MTU - 40u); T_EQ(ib.mss, MT_LO_MTU - 40u);  /* loopback MSS 536 */
    T_EQ(mt_next_deadline(s, now) == 0, 0);
    /* 20000 bytes b -> a, read as they arrive */
    for (i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 13u + (i >> 8));
    for (step = 0; step < 20000 && rcvd < sizeof data; step++) {
        if (sent < sizeof data) {
            int32_t r = tsend(b, data + sent, (uint32_t)(sizeof data - sent) > 3000u ? 3000u
                              : (uint32_t)(sizeof data - sent), 1, 0, now);
            if (r > 0) sent += (uint32_t)r;
        }
        rcvd += mt_tcp_recv(a, got + rcvd, sizeof got - rcvd, now);
        run(s, &now, now + 1);
    }
    T_EQ(sent, sizeof data); T_EQ(rcvd, sizeof data);
    T_MEMEQ(got, data, sizeof data);
    T_EQ(st->tcp_rexmt, 0);
    /* close both ends: FIN exchange, TIME_WAIT, both terminate */
    T_EQ(mt_tcp_close(b, now), MT_OK);
    run(s, &now, now + 50);
    T_EQ(mt_tcp_close(a, now), MT_OK);
    run(s, &now, now + 10000);
    mt_tcp_get_info(a, &ia); mt_tcp_get_info(b, &ib);
    T_EQ(ia.state, MT_TCP_CLOSED); T_EQ(ib.state, MT_TCP_CLOSED);
    T_EQ(st->tcp_conns_open, 0);
    T_EQ(fl.n, 0); T_EQ(st->eth_out, 0); T_EQ(st->tcp_resets_out, 0);
    mt_tcp_release(a); mt_tcp_release(b);
    /* 127.0.0.1: the connection's addresses stay 127.0.0.1 on both ends */
    memset(&ea, 0, sizeof ea); memset(&eb, 0, sizeof eb);
    a = mt_tcp_create(s, ra, sizeof ra, rec_cb, &ea);
    b = mt_tcp_create(s, rb, sizeof rb, rec_cb, &eb);
    T_CHECK(a && b);
    if (!a || !b) return;
    T_EQ(mt_tcp_listen(a, 0, 0, 23), MT_OK);
    T_EQ(mt_tcp_open(b, LO1, 23, 0, now), MT_OK);
    run(s, &now, now + 10);
    mt_tcp_get_info(a, &ia); mt_tcp_get_info(b, &ib);
    T_EQ(ia.state, MT_TCP_ESTABLISHED); T_EQ(ib.state, MT_TCP_ESTABLISHED);
    T_EQ(ia.rip, LO1); T_EQ(ia.lip, LO1); T_EQ(ib.lip, LO1);
    T_EQ(tsend(b, (const uint8_t *)"hello", 5, 1, 0, now), 5);
    run(s, &now, now + 10);
    T_EQ(mt_tcp_recv(a, got, sizeof got, now), 5);
    T_MEMEQ(got, "hello", 5);
    mt_tcp_abort(b, now);
    run(s, &now, now + 10);
    mt_tcp_get_info(a, &ia);
    T_EQ(ia.state, MT_TCP_CLOSED);                             /* the RST came back to a */
    mt_tcp_release(a); mt_tcp_release(b);
    T_EQ(fl.n, 0);
}

/* ---- unconfigured / address change ---- */

static void test_loopback_unconfigured_drops(void) {
    fake_link fl; mt_stack *s = tstack_new(&fl, 0);
    static uint8_t ub[2048];
    mt_udp *u = mt_udp_create(s, 9, ub, sizeof ub, NULL, NULL);
    mt_ifconfig c;
    ecb e;
    const mt_stats *st = mt_get_stats(s);
    T_CHECK(u != NULL);
    if (!u) return;
    T_EQ(mt_icmp_echo(s, LO1, 1, 1, NULL, 0, 0), MT_ERR_UNCONFIGURED);
    T_EQ(mt_icmp_echo(s, 0, 1, 1, NULL, 0, 0), MT_ERR_UNCONFIGURED);
    T_EQ(mt_udp_send(u, LO1, 9, (const uint8_t *)"x", 1, 1, 0), MT_ERR_UNCONFIGURED);
    T_EQ(st->ip_lo_out, 0);
    T_EQ(mt_next_deadline(s, 0), MT_NO_DEADLINE);
    T_EQ(fl.n, 0);
    /* configured: two datagrams queued, then the address changes: they are discarded (their
     * source is the old address), not delivered */
    mt_get_config(s, &c);
    c.addr = T_ME;
    mt_set_config(s, &c);
    memset(&e, 0, sizeof e);
    mt_set_echo_cb(s, echo_cb, &e);
    T_EQ(mt_icmp_echo(s, T_ME, 1, 1, NULL, 0, 0), MT_OK);
    T_EQ(mt_udp_send(u, T_ME, 9, (const uint8_t *)"x", 1, 1, 0), MT_OK);
    T_EQ(st->ip_lo_out, 2);
    c.addr = 0;
    mt_set_config(s, &c);                                     /* unconfigured again */
    T_EQ(st->ip_lo_drops, 2);
    T_EQ(mt_next_deadline(s, 0), MT_NO_DEADLINE);
    mt_tick(s, 0);
    T_EQ(mt_udp_queued(u), 0); T_EQ(e.n, 0); T_EQ(st->ip_in, 0);
    c.addr = T_ME;
    mt_set_config(s, &c);
    T_EQ(mt_udp_send(u, T_ME, 9, (const uint8_t *)"x", 1, 1, 0), MT_OK);
    c.addr = T_ME + 1;                                        /* another address: discarded too */
    mt_set_config(s, &c);
    mt_tick(s, 0);
    T_EQ(mt_udp_queued(u), 0); T_EQ(st->ip_lo_drops, 3);
    T_EQ(fl.n, 0);
    mt_udp_release(u);
}

/* ---- no amplification, no loops ---- */

static void test_loopback_no_amplification(void) {
    fake_link fl; mt_stack *s = tstack_new(&fl, T_ME);
    static uint8_t ub[2048], tb[4096];
    ecb e; uint32_t now = 0;
    rec ev; mt_tcp_info ti;
    mt_udp *u = mt_udp_create(s, 0, ub, sizeof ub, NULL, NULL);
    mt_tcp *t = mt_tcp_create(s, tb, sizeof tb, rec_cb, &ev);
    const mt_stats *st = mt_get_stats(s);
    T_CHECK(u && t);
    if (!u || !t) return;
    memset(&e, 0, sizeof e); memset(&ev, 0, sizeof ev);
    mt_set_echo_cb(s, echo_cb, &e);
    /* an echo request makes exactly one reply; the reply makes nothing */
    T_EQ(mt_icmp_echo(s, T_ME, 1, 1, NULL, 0, now), MT_OK);
    T_EQ(run(s, &now, 5000), 2);
    T_EQ(e.n, 1); T_EQ(st->ip_lo_out, 2); T_EQ(st->icmp_out, 2); T_EQ(st->icmp_in, 2);
    /* UDP to a closed port of ours: dropped, no ICMP error toward ourselves */
    T_EQ(mt_udp_send(u, T_ME, 4444, (const uint8_t *)"x", 1, 1, now), MT_OK);
    T_EQ(run(s, &now, 10000), 1);
    T_EQ(st->udp_noport, 1); T_EQ(st->ip_lo_out, 3); T_EQ(st->icmp_out, 2);
    /* SYN to a closed port of ours: one RST back, which is not answered */
    T_EQ(mt_tcp_open(t, T_ME, 4445, 0, now), MT_OK);
    T_EQ(run(s, &now, 15000), 2);
    mt_tcp_get_info(t, &ti);
    T_EQ(ti.state, MT_TCP_CLOSED);
    T_EQ(st->ip_lo_out, 5); T_EQ(st->tcp_resets_out, 1); T_EQ(st->tcp_resets_in, 1);
    T_EQ(ev.n, 1); T_EQ(ev.e[0].ev, MT_TCP_EV_TERMINATE);
    /* broadcast and multicast are never looped back (they go to the wire as before) */
    T_EQ(mt_icmp_echo(s, 0xFFFFFFFFu, 1, 2, NULL, 0, now), MT_OK);
    T_EQ(mt_icmp_echo(s, T_ME | 0xFFu, 1, 3, NULL, 0, now), MT_OK);
    T_EQ(mt_udp_send(u, 0xE0000001u, 9, (const uint8_t *)"x", 1, 1, now), MT_OK);
    T_EQ(st->ip_lo_out, 5);
    T_EQ(fl.n, 3);                                            /* 2 broadcasts + ARP for the gateway */
    T_EQ(mt_next_deadline(s, now) == 0, 0);
    T_EQ(e.n, 1);
    mt_udp_release(u); mt_tcp_release(t);
}

/* ---- a full FIFO is a refusal: TCP stalls and retries (Task 2), nothing lost ---- */

static void test_loopback_full_slot_retries(void) {
    fake_link fl; mt_stack *s = tstack_new(&fl, T_ME);
    static uint8_t ra[16384], rb[16384], data[6000], got[6000];
    rec ea, eb; mt_tcp_info ib;
    uint32_t now = 0, rcvd = 0, i, una0;
    int step;
    mt_tcp *a, *b;
    const mt_stats *st = mt_get_stats(s);
    memset(&ea, 0, sizeof ea); memset(&eb, 0, sizeof eb);
    a = mt_tcp_create(s, ra, sizeof ra, rec_cb, &ea);
    b = mt_tcp_create(s, rb, sizeof rb, rec_cb, &eb);
    T_CHECK(a && b);
    if (!a || !b) return;
    T_EQ(mt_tcp_listen(a, 0, 0, 5000), MT_OK);
    T_EQ(mt_tcp_open(b, T_ME, 5000, 0, now), MT_OK);
    run(s, &now, 10);
    mt_tcp_get_info(b, &ib);
    T_EQ(ib.state, MT_TCP_ESTABLISHED);
    una0 = ib.snd_una;
    for (i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i ^ (i >> 7));
    /* one call, 6000 bytes: the initial window allows more than 3 segments, the FIFO holds 3 */
    T_EQ(tsend(b, data, sizeof data, 1, 0, now), (int32_t)sizeof data);
    T_EQ(st->ip_lo_out, 3 + 3);                               /* handshake + 3 data segments */
    T_EQ(st->ip_lo_drops, 1); T_EQ(st->tcp_tx_stalls, 1);
    mt_tcp_get_info(b, &ib);
    T_EQ(ib.snd_nxt - una0, 3u * (MT_LO_MTU - 40u));          /* the refused segment is not "sent" */
    for (step = 0; step < 2000 && rcvd < sizeof data; step++) {
        rcvd += mt_tcp_recv(a, got + rcvd, sizeof got - rcvd, now);
        run(s, &now, now + 1);
    }
    T_EQ(rcvd, sizeof data);
    T_MEMEQ(got, data, sizeof data);
    T_EQ(st->tcp_rexmt, 0);                                   /* retried by the stall, not the RTO */
    T_CHECK(st->tcp_tx_stalls >= 1);
    T_EQ(st->tcp_tx_stalls, st->ip_lo_drops);                 /* every refusal was a TCP frame */
    T_EQ(fl.n, 0);
    mt_tcp_abort(b, now); mt_tcp_abort(a, now);
    mt_tcp_release(a); mt_tcp_release(b);
}

/* ---- wire vs loopback: our own source is accepted only from the FIFO ---- */

static void test_loopback_wire_spoof_dropped(void) {
    fake_link fl; mt_stack *s = tstack_new(&fl, T_ME);
    static uint8_t ub[2048];
    uint8_t f[200], seg[12];
    uint16_t n;
    mt_udp *u = mt_udp_create(s, 9, ub, sizeof ub, NULL, NULL);
    const mt_stats *st = mt_get_stats(s);
    uint32_t drops0;
    T_CHECK(u != NULL);
    if (!u) return;
    mt_wr16(seg, 1234); mt_wr16(seg + 2, 9); mt_wr16(seg + 4, 12); mt_wr16(seg + 6, 0);
    memcpy(seg + 8, "abcd", 4);
    drops0 = st->ip_in_drops;
    n = mk_ip(f, T_ME, T_ME, 17, 1, 0, seg, 12);      /* our own address as source, from the wire */
    mt_input(s, f, n, 0);
    n = mk_ip(f, LO1, T_ME, 17, 2, 0, seg, 12);       /* 127/8 source from the wire */
    mt_input(s, f, n, 0);
    n = mk_ip(f, T_PEER, LO1, 17, 3, 0, seg, 12);     /* 127/8 destination from the wire */
    mt_input(s, f, n, 0);
    T_EQ(st->ip_in_drops, drops0 + 3); T_EQ(st->ip_in, 0); T_EQ(mt_udp_queued(u), 0);
    n = mk_ip(f, T_PEER, T_ME, 17, 4, 0, seg, 12);    /* a real peer: accepted */
    mt_input(s, f, n, 0);
    T_EQ(mt_udp_queued(u), 1); T_EQ(st->ip_in, 1);
    T_EQ(fl.n, 0);                                    /* nothing was answered */
    /* the same datagram from the FIFO is accepted */
    T_EQ(mt_udp_send(u, T_ME, 9, (const uint8_t *)"abcd", 4, 1, 0), MT_OK);
    mt_tick(s, 0);
    T_EQ(mt_udp_queued(u), 2); T_EQ(st->ip_in, 2);
    mt_udp_release(u);
}

/* ---- re-entrancy: a callback inside the drain may send to ourselves and reconfigure ---- */

typedef struct { mt_stack *s; int n, resend, reconf; } rcb;
static void re_cb(mt_udp *u, mt_udp_event ev, uint32_t arg, void *user) {
    rcb *c = (rcb *)user;
    mt_udp_dgram dg;
    (void)arg;
    if (ev != MT_UDP_EV_DATA) return;
    while (mt_udp_peek(u, &dg)) {
        c->n++;
        mt_udp_done(u);
        if (c->resend) { c->resend--; mt_udp_send(u, T_ME, mt_udp_port(u), (const uint8_t *)"r", 1, 1, 0); }
        if (c->reconf) {
            mt_ifconfig cf;
            c->reconf = 0;
            mt_get_config(c->s, &cf);
            cf.addr = T_ME + 1;
            mt_set_config(c->s, &cf);
            /* queued after the flush, inside the drain: waits for the next tick */
            mt_udp_send(u, T_ME + 1, mt_udp_port(u), (const uint8_t *)"n", 1, 1, 0);
        }
    }
}

static void test_loopback_reentrant_callbacks(void) {
    fake_link fl; mt_stack *s = tstack_new(&fl, T_ME);
    static uint8_t ub[4096];
    rcb c;
    mt_udp *u;
    const mt_stats *st = mt_get_stats(s);
    memset(&c, 0, sizeof c);
    c.s = s;
    u = mt_udp_create(s, 9, ub, sizeof ub, re_cb, &c);
    T_CHECK(u != NULL);
    if (!u) return;
    /* each datagram triggers one more: one per tick, never two inside one mt_tick */
    c.resend = 5;
    T_EQ(mt_udp_send(u, T_ME, 9, (const uint8_t *)"x", 1, 1, 0), MT_OK);
    mt_tick(s, 0); T_EQ(c.n, 1);
    mt_tick(s, 0); T_EQ(c.n, 2);
    mt_tick(s, 0); T_EQ(c.n, 3);
    { uint32_t now = 0; run(s, &now, 100); }
    T_EQ(c.n, 6); T_EQ(st->ip_lo_out, 6);
    /* three queued; the first one's callback changes the address: the other two are discarded,
     * the one being delivered finishes cleanly */
    c.n = 0; c.reconf = 1;
    T_EQ(mt_udp_send(u, T_ME, 9, (const uint8_t *)"1", 1, 1, 0), MT_OK);
    T_EQ(mt_udp_send(u, T_ME, 9, (const uint8_t *)"2", 1, 1, 0), MT_OK);
    T_EQ(mt_udp_send(u, T_ME, 9, (const uint8_t *)"3", 1, 1, 0), MT_OK);
    mt_tick(s, 0);
    T_EQ(c.n, 1); T_EQ(st->ip_lo_drops, 2);
    T_EQ(s->lo->n, 1);                       /* the datagram sent after the flush: next tick */
    T_EQ(mt_next_deadline(s, 0), 0);
    mt_tick(s, 0);
    T_EQ(c.n, 2);
    T_EQ(mt_next_deadline(s, 0), MT_NO_DEADLINE);
    /* the new address loops back too */
    T_EQ(mt_udp_send(u, T_ME + 1, 9, (const uint8_t *)"4", 1, 1, 0), MT_OK);
    mt_tick(s, 0);
    T_EQ(c.n, 3);
    T_EQ(fl.n, 0);
    mt_udp_release(u);
}

T_MAIN(test_loopback_ping, test_loopback_udp_echo, test_loopback_tcp_connect_send_close,
       test_loopback_unconfigured_drops, test_loopback_no_amplification,
       test_loopback_full_slot_retries, test_loopback_wire_spoof_dropped,
       test_loopback_reentrant_callbacks)
