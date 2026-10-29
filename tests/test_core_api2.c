#include "t.h"
#include "net.h"
#include "mt_int.h"
#include "mt_util.h"

#include "dnsb.h"

/* Plan 2 Task 4: DNS cancel / PTR / reserved slot, entropy, TCP listen on port 0. */

static void test_dns_cancel_no_callback_no_write(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    mt_dns_result r, snap; got g = {0};
    qv q; pkt p;
    T_EQ(mt_dns_query(s, "a.example", MT_DNS_Q_A, &r, on_done, &g, 1000), MT_DNS_IN_PROGRESS);
    get_q(&fl, 0, &q);
    T_CHECK(mt_dns_deadline(s, 1000) != MT_NO_DEADLINE);
    memset(&r, 0xAB, sizeof r);                       /* canary: any write would change it */
    snap = r;
    mt_dns_cancel(s, &r);
    T_EQ(mt_dns_deadline(s, 1000), MT_NO_DEADLINE);   /* retries stop too */
    begin(&p, q.id, 0x8180, "a.example", 1);
    count_an(&p, 1);
    rr_a(&p, 300, 0x0A000001u);
    reply(s, &q, &p, 1100);                           /* a late answer for the cancelled query */
    mt_tick(s, 30000);                                /* and no timeout */
    T_EQ(g.n, 0);
    T_MEMEQ(&r, &snap, sizeof r);
    T_EQ(fl.n, 1);
    mt_dns_cancel(s, &r);                             /* cancelling nothing is fine */
    mt_dns_cancel(s, NULL);
}

static void test_dns_cancel_unrelated_query_unaffected(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    mt_dns_result r1, r2; got g1 = {0}, g2 = {0};
    qv q1, q2; pkt p;
    T_EQ(mt_dns_query(s, "one.example", MT_DNS_Q_A, &r1, on_done, &g1, 0), MT_DNS_IN_PROGRESS);
    T_EQ(mt_dns_query(s, "two.example", MT_DNS_Q_A, &r2, on_done, &g2, 0), MT_DNS_IN_PROGRESS);
    get_q(&fl, 0, &q1); get_q(&fl, 1, &q2);
    mt_dns_cancel(s, &r1);
    begin(&p, q2.id, 0x8180, "two.example", 1);
    count_an(&p, 1);
    rr_a(&p, 300, 0x0A000002u);
    reply(s, &q2, &p, 50);
    T_EQ(g2.n, 1); T_EQ(g2.r.status, MT_DNS_OK); T_EQ(g2.r.addr[0], 0x0A000002u);
    T_EQ(g1.n, 0);
    /* the slot of the cancelled query is reusable */
    T_EQ(mt_dns_query(s, "three.example", MT_DNS_Q_A, &r1, on_done, &g1, 60), MT_DNS_IN_PROGRESS);
    mt_dns_cancel(s, &r1);
    T_EQ(mt_dns_deadline(s, 60), MT_NO_DEADLINE);
}

static void test_dns_reserved_slot_survives_full_udp(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    static uint8_t bufs[9][1024];
    mt_dns_result r; got g = {0};
    qv q; pkt p;
    int i;
    for (i = 0; i < 8; i++)
        T_CHECK(mt_udp_create(s, (uint16_t)(3000 + i), bufs[i], 1024, NULL, NULL) != NULL);
    T_CHECK(mt_udp_create(s, 3100, bufs[8], 1024, NULL, NULL) == NULL);     /* app limit is max_udp */
    T_EQ(mt_dns_query(s, "x.example", MT_DNS_Q_A, &r, on_done, &g, 0), MT_DNS_IN_PROGRESS);
    T_EQ(fl.n, 1);
    get_q(&fl, 0, &q);
    begin(&p, q.id, 0x8180, "x.example", 1);
    count_an(&p, 1);
    rr_a(&p, 300, 0x0A000009u);
    reply(s, &q, &p, 10);
    T_EQ(g.n, 1); T_EQ(g.r.addr[0], 0x0A000009u);
    /* the DNS endpoint never takes an app slot */
    T_CHECK(mt_udp_create(s, 3100, bufs[8], 1024, NULL, NULL) == NULL);
}

typedef struct { int n; mt_dns_status st; char name[300]; } pgot;
static void ptr_done(void *user, mt_dns_status st, const char *name) {
    pgot *g = (pgot *)user;
    g->n++; g->st = st; strcpy(g->name, name);
}

#define PTR_ADDR 0xC0A8320Au            /* 192.168.50.10 */
#define PTR_NAME "10.50.168.192.in-addr.arpa"

/* PTR answer for PTR_NAME: owner via the 0xC00C pointer, rdata "host" + a pointer into the
 * question's "in-addr.arpa" (offset 12 + 14), i.e. "host.in-addr.arpa". */
static void ptr_answer(pkt *p, uint16_t id, uint32_t ttl) {
    int at;
    begin(p, id, 0x8180, PTR_NAME, 12);
    count_an(p, 1);
    at = rr(p, 12, ttl);
    p8(p, 4); memcpy(p->b + p->n, "host", 4); p->n += 4;
    pptr(p, 26);
    endrr(p, at);
}

static void test_dns_ptr_roundtrip_and_cache(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    char buf[64]; pgot g = {0};
    qv q; pkt p;
    memset(buf, 'x', sizeof buf);
    T_EQ(mt_dns_ptr(s, PTR_ADDR, buf, sizeof buf, ptr_done, &g, 1000), MT_DNS_IN_PROGRESS);
    T_EQ(fl.n, 1);
    get_q(&fl, 0, &q);
    T_CHECK(q.ok); T_EQ(q.qtype, 12); T_EQ(q.dst, T_DNS);
    T_CHECK(strcmp(q.name, PTR_NAME) == 0);
    ptr_answer(&p, q.id, 600);
    reply(s, &q, &p, 1100);
    T_EQ(g.n, 1); T_EQ(g.st, MT_DNS_OK);
    T_CHECK(strcmp(buf, "host.in-addr.arpa") == 0);
    T_CHECK(strcmp(g.name, "host.in-addr.arpa") == 0);
    T_EQ(mt_dns_deadline(s, 1100), MT_NO_DEADLINE);
    /* cache hit: no frame, no callback, name in the buffer */
    memset(buf, 'y', sizeof buf);
    T_EQ(mt_dns_ptr(s, PTR_ADDR, buf, sizeof buf, ptr_done, &g, 2000), 0);
    T_CHECK(strcmp(buf, "host.in-addr.arpa") == 0);
    T_EQ(g.n, 1); T_EQ(fl.n, 1);
    /* a cached PTR is not an A entry: it is not found by name */
    {
        mt_dns_result r;
        T_EQ(mt_dns_query(s, "host.in-addr.arpa", MT_DNS_Q_A, &r, NULL, NULL, 2000), MT_DNS_IN_PROGRESS);
        mt_dns_cancel(s, &r);
    }
    /* another address misses */
    T_EQ(mt_dns_ptr(s, PTR_ADDR + 1, buf, sizeof buf, ptr_done, &g, 2000), MT_DNS_IN_PROGRESS);
    mt_dns_cancel(s, buf);
    /* expiry (TTL 600 s) */
    T_EQ(mt_dns_ptr(s, PTR_ADDR, buf, sizeof buf, ptr_done, &g, 1100 + 601000u), MT_DNS_IN_PROGRESS);
    mt_dns_cancel(s, buf);
}

static void test_dns_ptr_cap_truncates(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    char buf[12]; pgot g = {0};
    qv q; pkt p;
    memset(buf, 'x', sizeof buf);
    T_EQ(mt_dns_ptr(s, PTR_ADDR, buf, 6, ptr_done, &g, 0), MT_DNS_IN_PROGRESS);
    get_q(&fl, 0, &q);
    ptr_answer(&p, q.id, 300);
    reply(s, &q, &p, 10);
    T_EQ(g.n, 1); T_EQ(g.st, MT_DNS_OK);
    T_CHECK(memcmp(buf, "host.\0", 6) == 0);              /* 5 chars + NUL, nothing past cap */
    T_EQ(buf[6], 'x');
    memset(buf, 'x', sizeof buf);
    T_EQ(mt_dns_ptr(s, PTR_ADDR, buf, 4, ptr_done, &g, 20), 0);   /* cache hit, same rule */
    T_CHECK(memcmp(buf, "hos\0", 4) == 0);
    T_EQ(buf[4], 'x');
    T_EQ(mt_dns_ptr(s, PTR_ADDR, buf, 1, ptr_done, &g, 20), 0);
    T_EQ(buf[0], 0);
    T_EQ(mt_dns_ptr(s, PTR_ADDR, buf, 0, ptr_done, &g, 20), -MT_DNS_BAD_NAME);   /* no room at all */
    T_EQ(mt_dns_ptr(s, PTR_ADDR, NULL, 8, ptr_done, &g, 20), -MT_DNS_BAD_NAME);
}

static void test_dns_ptr_negative_and_busy(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    char buf[32], buf2[32]; pgot g = {0};
    qv q; pkt p;
    memset(buf, 'x', sizeof buf);
    T_EQ(mt_dns_ptr(s, PTR_ADDR, buf, sizeof buf, ptr_done, &g, 0), MT_DNS_IN_PROGRESS);
    /* one reverse lookup at a time (the shared scratch result) */
    T_EQ(mt_dns_ptr(s, PTR_ADDR + 1, buf2, sizeof buf2, ptr_done, &g, 0), -MT_DNS_BUSY);
    get_q(&fl, 0, &q);
    begin(&p, q.id, 0x8183, PTR_NAME, 12);                 /* NXDOMAIN */
    reply(s, &q, &p, 10);
    T_EQ(g.n, 1); T_EQ(g.st, MT_DNS_NOT_FOUND);
    T_EQ(buf[0], 0);
    /* not cached; the scratch is free again */
    T_EQ(mt_dns_ptr(s, PTR_ADDR, buf, sizeof buf, ptr_done, &g, 20), MT_DNS_IN_PROGRESS);
    /* timeout reports TIMEOUT and clears the buffer */
    mt_tick(s, 20 + 14000);
    T_EQ(g.n, 2); T_EQ(g.st, MT_DNS_TIMEOUT); T_EQ(buf[0], 0);
    /* no server */
    {
        fake_link fl2; mt_stack *s2 = tstack_new(&fl2, T_ME);
        T_EQ(mt_dns_ptr(s2, PTR_ADDR, buf, sizeof buf, ptr_done, &g, 0), -MT_DNS_NO_SERVER);
    }
}

static void test_dns_ptr_cancel_no_write(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    char buf[32], snap[32]; pgot g = {0};
    qv q; pkt p;
    T_EQ(mt_dns_ptr(s, PTR_ADDR, buf, sizeof buf, ptr_done, &g, 0), MT_DNS_IN_PROGRESS);
    memset(buf, 0xAB, sizeof buf);                         /* canary after the call */
    memcpy(snap, buf, sizeof buf);
    get_q(&fl, 0, &q);
    mt_dns_cancel(s, buf);
    ptr_answer(&p, q.id, 300);
    reply(s, &q, &p, 10);
    mt_tick(s, 40000);
    T_EQ(g.n, 0);
    T_MEMEQ(buf, snap, sizeof buf);
    T_EQ(mt_dns_deadline(s, 40000), MT_NO_DEADLINE);
    /* a cancelled PTR frees the scratch for the next one */
    T_EQ(mt_dns_ptr(s, PTR_ADDR, buf, sizeof buf, ptr_done, &g, 50000), MT_DNS_IN_PROGRESS);
}

static void test_dns_entropy_ids_and_port(void) {
    fake_link f1, f2, f3; mt_stack *a, *b, *c;
    mt_dns_result r1, r2, r3; qv q1, q2, q3;
    a = setup(&f1, NULL); mt_set_entropy(a, 11, 22);
    mt_dns_query(a, "x.example", MT_DNS_Q_A, &r1, NULL, NULL, 0);
    get_q(&f1, 0, &q1);
    b = setup(&f2, NULL); mt_set_entropy(b, 11, 22);
    mt_dns_query(b, "x.example", MT_DNS_Q_A, &r2, NULL, NULL, 0);
    get_q(&f2, 0, &q2);
    c = setup(&f3, NULL); mt_set_entropy(c, 333, 444);
    mt_dns_query(c, "x.example", MT_DNS_Q_A, &r3, NULL, NULL, 0);
    get_q(&f3, 0, &q3);
    T_EQ(q1.id, q2.id); T_EQ(q1.sport, q2.sport);          /* deterministic for equal entropy */
    T_CHECK(q1.id != q3.id);
    T_CHECK(q1.sport != q3.sport);
    T_CHECK(q1.sport >= 49152 && q3.sport >= 49152);
}

static void test_isn_differs_per_tuple_same_ms(void) {
    fake_link f1, f2; mt_stack *s, *s2, *s3;
    mt_tcp *t1, *t2, *t3, *t4;
    static uint8_t b[4][1024];
    tcpv v1, v2, v3, v4;
    s = tstack_new(&f1, T_ME);
    mt_set_entropy(s, 1000, 0x1234567u);
    mt_arp_insert(s, T_PEER, T_PEER_MAC, 0);
    mt_arp_insert(s, T_PEER + 1, T_PEER_MAC, 0);
    t1 = mt_tcp_create(s, b[0], 1024, NULL, NULL);
    t2 = mt_tcp_create(s, b[1], 1024, NULL, NULL);
    t3 = mt_tcp_create(s, b[2], 1024, NULL, NULL);
    t4 = mt_tcp_create(s, b[3], 1024, NULL, NULL);
    T_EQ(mt_tcp_open(t1, T_PEER, 23, 5000, 777), MT_OK);
    T_EQ(mt_tcp_open(t2, T_PEER, 24, 5000, 777), MT_OK);       /* other remote port */
    T_EQ(mt_tcp_open(t3, T_PEER + 1, 23, 5000, 777), MT_OK);   /* other remote address */
    T_EQ(mt_tcp_open(t4, T_PEER, 23, 5001, 777), MT_OK);       /* other local port */
    T_EQ(f1.n, 4);
    T_CHECK(view_tcp(f1.f[0], f1.len[0], &v1)); T_CHECK(view_tcp(f1.f[1], f1.len[1], &v2));
    T_CHECK(view_tcp(f1.f[2], f1.len[2], &v3)); T_CHECK(view_tcp(f1.f[3], f1.len[3], &v4));
    T_CHECK(v1.seq != v2.seq); T_CHECK(v1.seq != v3.seq); T_CHECK(v1.seq != v4.seq);
    T_CHECK(v2.seq != v3.seq); T_CHECK(v2.seq != v4.seq); T_CHECK(v3.seq != v4.seq);
    /* same entropy and tuple: same ISN; the time term still advances 250 per ms */
    s2 = tstack_new(&f2, T_ME);
    mt_set_entropy(s2, 1000, 0x1234567u);
    mt_arp_insert(s2, T_PEER, T_PEER_MAC, 0);
    T_EQ(mt_tcp_open(mt_tcp_create(s2, b[0], 1024, NULL, NULL), T_PEER, 23, 5000, 777), MT_OK);
    T_CHECK(view_tcp(f2.f[0], f2.len[0], &v2));
    T_EQ(v2.seq, v1.seq);
    s3 = tstack_new(&f2, T_ME);
    mt_set_entropy(s3, 1000, 0x1234567u);
    mt_arp_insert(s3, T_PEER, T_PEER_MAC, 0);
    T_EQ(mt_tcp_open(mt_tcp_create(s3, b[0], 1024, NULL, NULL), T_PEER, 23, 5000, 778), MT_OK);
    T_CHECK(view_tcp(f2.f[0], f2.len[0], &v3));
    T_EQ(v3.seq, v1.seq + 250u);
    /* another key: another ISN */
    s3 = tstack_new(&f2, T_ME);
    mt_set_entropy(s3, 1000, 0x7654321u);
    mt_arp_insert(s3, T_PEER, T_PEER_MAC, 0);
    T_EQ(mt_tcp_open(mt_tcp_create(s3, b[0], 1024, NULL, NULL), T_PEER, 23, 5000, 777), MT_OK);
    T_CHECK(view_tcp(f2.f[0], f2.len[0], &v3));
    T_CHECK(v3.seq != v1.seq);
}

static int get_lport(mt_tcp *t) { mt_tcp_info in; mt_tcp_get_info(t, &in); return in.lport; }

static void test_listen_port0_assigns_unused(void) {
    fake_link fl; mt_stack *s = tstack_new(&fl, T_ME);
    static uint8_t b[1024];
    static const uint8_t mssopt[4] = {2, 4, 5, 0xB4};
    mt_tcp *t = mt_tcp_create(s, b, 1024, NULL, NULL);
    uint8_t f[200];
    uint16_t n;
    tcpv v;
    int p;
    mt_arp_insert(s, T_PEER, T_PEER_MAC, 0);
    T_EQ(mt_tcp_listen(t, 0, 0, 0), MT_OK);
    p = get_lport(t);
    T_CHECK(p >= 1024 && p <= 2047);
    n = mk_tcp(f, T_PEER, 4000, T_ME, (uint16_t)p, 77, 0, TF_SYN, 8192, mssopt, 4, NULL, 0);
    mt_input(s, f, n, 5);
    T_EQ(fl.n, 1);
    T_CHECK(view_tcp(fl.f[0], fl.len[0], &v));
    T_EQ(v.flags, TF_SYN | TF_ACK); T_EQ(v.sport, p);
    fake_clear(&fl);
    /* a SYN to any other port is not accepted: RST */
    n = mk_tcp(f, T_PEER, 4001, T_ME, (uint16_t)(p + 1), 77, 0, TF_SYN, 8192, NULL, 0, NULL, 0);
    mt_input(s, f, n, 6);
    T_EQ(fl.n, 1);
    T_CHECK(view_tcp(fl.f[0], fl.len[0], &v));
    T_CHECK(v.flags & TF_RST);
}

static void test_listen_port0_skips_in_use(void) {
    fake_link fl; mt_stack *s = tstack_new(&fl, T_ME);
    static uint8_t b[6][1024];
    mt_tcp *t[6];
    int i;
    for (i = 0; i < 6; i++) t[i] = mt_tcp_create(s, b[i], 1024, NULL, NULL);
    mt_arp_insert(s, T_PEER, T_PEER_MAC, 0);
    s->lis_eph = 1500;
    T_EQ(mt_tcp_listen(t[0], 0, 0, 1500), MT_OK);          /* explicit listener on the candidate */
    T_EQ(mt_tcp_listen(t[1], 0, 0, 1501), MT_OK);
    T_EQ(mt_tcp_open(t[2], T_PEER, 23, 1502, 0), MT_OK);   /* a connection using the next one */
    T_EQ(mt_tcp_listen(t[3], 0, 0, 0), MT_OK);
    T_EQ(get_lport(t[3]), 1503);
    T_EQ(mt_tcp_listen(t[4], 0, 0, 0), MT_OK);
    T_EQ(get_lport(t[4]), 1504);
    mt_tcp_close(t[3], 0);                                  /* CLOSED frees 1503 ... */
    s->lis_eph = 1503;
    T_EQ(mt_tcp_listen(t[5], 0, 0, 0), MT_OK);
    T_EQ(get_lport(t[5]), 1503);                            /* ... for reuse */
    /* the range wraps from 2047 back to 1024 */
    mt_tcp_close(t[5], 0);
    s->lis_eph = 2047;
    T_EQ(mt_tcp_listen(t[5], 0, 0, 0), MT_OK);
    T_EQ(get_lport(t[5]), 2047);
    mt_tcp_close(t[5], 0);
    T_EQ(mt_tcp_listen(t[5], 0, 0, 0), MT_OK);
    T_EQ(get_lport(t[5]), 1024);
}

static void test_listen_same_port_twice_allowed(void) {
    fake_link fl; mt_stack *s = tstack_new(&fl, T_ME);
    static uint8_t b[2][1024];
    mt_tcp *a = mt_tcp_create(s, b[0], 1024, NULL, NULL), *c = mt_tcp_create(s, b[1], 1024, NULL, NULL);
    uint8_t f[200];
    uint16_t n;
    mt_tcp_info ia, ic;
    mt_arp_insert(s, T_PEER, T_PEER_MAC, 0);
    T_EQ(mt_tcp_listen(a, 0, 0, 3000), MT_OK);
    T_EQ(mt_tcp_listen(c, 0, 0, 3000), MT_OK);             /* as MacTCP: allowed */
    n = mk_tcp(f, T_PEER, 4000, T_ME, 3000, 77, 0, TF_SYN, 8192, NULL, 0, NULL, 0);
    mt_input(s, f, n, 1);
    n = mk_tcp(f, T_PEER, 4001, T_ME, 3000, 99, 0, TF_SYN, 8192, NULL, 0, NULL, 0);
    mt_input(s, f, n, 2);
    mt_tcp_get_info(a, &ia); mt_tcp_get_info(c, &ic);
    T_EQ(ia.state, MT_TCP_SYN_RCVD); T_EQ(ic.state, MT_TCP_SYN_RCVD);
    T_EQ(ia.rport, 4000); T_EQ(ic.rport, 4001);
}

T_MAIN(test_dns_cancel_no_callback_no_write, test_dns_cancel_unrelated_query_unaffected,
       test_dns_reserved_slot_survives_full_udp, test_dns_ptr_roundtrip_and_cache,
       test_dns_ptr_cap_truncates, test_dns_ptr_negative_and_busy, test_dns_ptr_cancel_no_write,
       test_dns_entropy_ids_and_port, test_isn_differs_per_tuple_same_ms,
       test_listen_port0_assigns_unused, test_listen_port0_skips_in_use,
       test_listen_same_port_twice_allowed)
