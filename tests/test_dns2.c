#include "dnsb.h"

/* Resolve `name` with one A record (TTL 300); asked = the question name that went on the wire. */
static void answer_a(mt_stack *s, fake_link *fl, const char *name, mt_ip4 ip, uint32_t now,
                     char asked[256]) {
    mt_dns_result r; got g = {0}; qv q; pkt p;
    int idx = fl->n;
    T_EQ(mt_dns_query(s, name, MT_DNS_Q_A, &r, on_done, &g, now), MT_DNS_IN_PROGRESS);
    get_q(fl, idx, &q);
    strcpy(asked, q.name);
    begin(&p, q.id, 0x8180, q.name, 1);
    count_an(&p, 1); rr_a(&p, 300, ip);
    reply(s, &q, &p, now);
    T_EQ(g.n, 1); T_EQ(g.r.status, MT_DNS_OK);
}

static void test_dns2_abs_not_search(void) {       /* (a) nas.lan cached, then "nas." */
    fake_link fl; mt_stack *s = setup(&fl, "lan");
    mt_dns_result r; got g = {0}; qv q; char a[256];
    answer_a(s, &fl, "nas", 0x0A0A0A0Au, 0, a);
    T_CHECK(strcmp(a, "nas.lan") == 0);
    T_EQ(mt_dns_query(s, "nas", MT_DNS_Q_A, &r, on_done, &g, 10), 0);       /* hits nas.lan */
    T_EQ(r.addr[0], 0x0A0A0A0Au);
    fake_clear(&fl);
    T_EQ(mt_dns_query(s, "nas.", MT_DNS_Q_A, &r, on_done, &g, 20), MT_DNS_IN_PROGRESS);
    get_q(&fl, 0, &q);
    T_CHECK(strcmp(q.name, "nas") == 0);
}

static void test_dns2_search_not_abs(void) {       /* (b) "nas." cached, then "nas" */
    fake_link fl; mt_stack *s = setup(&fl, "lan");
    mt_dns_result r; got g = {0}; qv q; char a[256];
    answer_a(s, &fl, "nas.", 0x0B0B0B0Bu, 0, a);
    T_CHECK(strcmp(a, "nas") == 0);
    T_EQ(mt_dns_query(s, "nas.", MT_DNS_Q_A, &r, on_done, &g, 10), 0);
    T_EQ(r.addr[0], 0x0B0B0B0Bu);
    fake_clear(&fl);
    T_EQ(mt_dns_query(s, "nas", MT_DNS_Q_A, &r, on_done, &g, 20), MT_DNS_IN_PROGRESS);
    get_q(&fl, 0, &q);
    T_CHECK(strcmp(q.name, "nas.lan") == 0);
}

static void test_dns2_domain_change(void) {        /* (c) */
    fake_link fl; mt_stack *s = setup(&fl, "lan");
    mt_dns_result r; got g = {0}; qv q; char a[256]; mt_ifconfig c;
    answer_a(s, &fl, "nas", 0x0A0A0A0Au, 0, a);
    mt_get_config(s, &c);
    strcpy(c.domain, "corp");
    mt_set_config(s, &c);
    fake_clear(&fl);
    T_EQ(mt_dns_query(s, "nas", MT_DNS_Q_A, &r, on_done, &g, 10), MT_DNS_IN_PROGRESS);
    get_q(&fl, 0, &q);
    T_CHECK(strcmp(q.name, "nas.corp") == 0);
    memset(c.domain, 0, sizeof c.domain);          /* no domain: bare "nas" is not nas.lan */
    mt_set_config(s, &c);
    fake_clear(&fl);
    T_EQ(mt_dns_query(s, "nas", MT_DNS_Q_A, &r, on_done, &g, 11), MT_DNS_IN_PROGRESS);
    get_q(&fl, 0, &q);
    T_CHECK(strcmp(q.name, "nas") == 0);
}

static void test_dns2_case_insensitive(void) {     /* (d) */
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    mt_dns_result r; got g = {0}; char a[256];
    answer_a(s, &fl, "Example.COM", 0x01020304u, 0, a);
    fake_clear(&fl);
    T_EQ(mt_dns_query(s, "example.com", MT_DNS_Q_A, &r, on_done, &g, 10), 0);
    T_EQ(r.addr[0], 0x01020304u);
    T_EQ(mt_dns_query(s, "EXAMPLE.Com", MT_DNS_Q_A, &r, on_done, &g, 10), 0);
    T_EQ(r.addr[0], 0x01020304u);
    T_EQ(fl.n, 0);
}

static void test_dns2_clock_wrap(void) {           /* expired entries must not revive */
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    mt_dns_result r; got g = {0}; char a[256];
    answer_a(s, &fl, "old.example", 0x01020304u, 0, a);       /* TTL 300 s -> exp 300000 */
    fake_clear(&fl);
    T_EQ(mt_dns_query(s, "old.example", MT_DNS_Q_A, &r, on_done, &g, 300000u + 2147483648u),
         MT_DNS_IN_PROGRESS);
    T_EQ(fl.n, 1);
}

static void test_dns2_rejected_leaves_out(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    mt_dns_result r, snap; got g = {0}; qv q; pkt p; int at;
    memset(&r, 0x5A, sizeof r);
    T_EQ(mt_dns_query(s, "spoof.example", MT_DNS_Q_A, &r, on_done, &g, 0), MT_DNS_IN_PROGRESS);
    T_EQ(r.status, MT_DNS_PENDING);
    snap = r;
    get_q(&fl, 0, &q);
    /* ID matches, question wrong */
    begin(&p, q.id, 0x8180, "other.example", 1);
    count_an(&p, 1); rr_a(&p, 60, 0x01010101u);
    reply(s, &q, &p, 10);
    T_MEMEQ(&r, &snap, sizeof r); T_EQ(r.status, MT_DNS_PENDING);
    /* announces an answer but carries none */
    begin(&p, q.id, 0x8180, "spoof.example", 1);
    count_an(&p, 1);
    reply(s, &q, &p, 11);
    T_MEMEQ(&r, &snap, sizeof r);
    /* good A record then a damaged one (rdlength past the end) */
    begin(&p, q.id, 0x8180, "spoof.example", 1);
    count_an(&p, 2); rr_a(&p, 60, 0x01010101u);
    at = rr(&p, 1, 60); p32(&p, 0x02020202u); mt_wr16(p.b + at, 40);
    reply(s, &q, &p, 12);
    T_MEMEQ(&r, &snap, sizeof r); T_EQ(r.status, MT_DNS_PENDING);
    /* CNAME to a name, then a damaged record */
    begin(&p, q.id, 0x8180, "spoof.example", 1);
    count_an(&p, 2);
    at = rr(&p, 5, 60); pname(&p, "evil.example"); endrr(&p, at);
    pptr(&p, 12);
    reply(s, &q, &p, 13);
    T_MEMEQ(&r, &snap, sizeof r);
    T_EQ(g.n, 0);
    begin(&p, q.id, 0x8180, "spoof.example", 1);
    count_an(&p, 1); rr_a(&p, 60, 0x03030303u);
    reply(s, &q, &p, 14);
    T_EQ(g.n, 1); T_EQ(g.r.status, MT_DNS_OK); T_EQ(g.r.naddr, 1); T_EQ(g.r.addr[0], 0x03030303u);
    T_EQ(r.status, MT_DNS_OK);
}

static void test_dns2_negative_retry_pending(void) {
    fake_link fl; mt_stack *s = setup(&fl, "lan");
    mt_dns_result r, snap; got g = {0}; qv q; pkt p; int at;
    memset(&r, 0x5A, sizeof r);
    T_EQ(mt_dns_query(s, "nas", MT_DNS_Q_A, &r, on_done, &g, 0), MT_DNS_IN_PROGRESS);
    snap = r;
    T_EQ(snap.status, MT_DNS_PENDING);
    get_q(&fl, 0, &q);
    /* nas.lan: a CNAME chain but no address (leaves partial data in `out` while parsing) */
    begin(&p, q.id, 0x8180, "nas.lan", 1);
    count_an(&p, 1);
    at = rr(&p, 5, 60); pname(&p, "elsewhere.lan"); endrr(&p, at);
    reply(s, &q, &p, 10);
    T_EQ(fl.n, 2); T_EQ(g.n, 0);
    T_MEMEQ(&r, &snap, sizeof r); T_EQ(r.status, MT_DNS_PENDING);
    mt_tick(s, 2010);                                      /* retransmit leaves `out` alone too */
    T_EQ(fl.n, 3);
    T_MEMEQ(&r, &snap, sizeof r);
    /* the bare question gets the real answer */
    get_q(&fl, 1, &q);
    T_CHECK(strcmp(q.name, "nas") == 0);
    begin(&p, q.id, 0x8180, "nas", 1);
    count_an(&p, 1); rr_a(&p, 60, 0x0C0C0C0Cu);
    reply(s, &q, &p, 2020);
    T_EQ(g.n, 1); T_EQ(g.r.status, MT_DNS_OK); T_EQ(g.r.naddr, 1); T_EQ(g.r.addr[0], 0x0C0C0C0Cu);
    T_EQ(r.status, MT_DNS_OK);
}

static void fill(char *d, size_t n) { memset(d, 'a', n); d[n] = 0; }

static void test_dns2_cache_name_boundary(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    mt_dns_result r; got g = {0}; char nm[80], a[256];
    fill(nm, 61); strcpy(nm + 61, ".b");                   /* 63 chars: cacheable */
    answer_a(s, &fl, nm, 0x01010101u, 0, a);
    T_EQ(mt_dns_query(s, nm, MT_DNS_Q_A, &r, on_done, &g, 5), 0);
    fill(nm, 62); strcpy(nm + 62, ".b");                   /* 64 chars: resolves, not cached */
    answer_a(s, &fl, nm, 0x02020202u, 0, a);
    T_EQ(mt_dns_query(s, nm, MT_DNS_Q_A, &r, on_done, &g, 5), MT_DNS_IN_PROGRESS);
    /* with a domain the FQDN counts: 59 + ".lan" = 63 cached, 60 + ".lan" = 64 not */
    s = setup(&fl, "lan");
    fill(nm, 59);
    answer_a(s, &fl, nm, 0x03030303u, 0, a);
    T_EQ(mt_dns_query(s, nm, MT_DNS_Q_A, &r, on_done, &g, 5), 0);
    T_EQ(r.addr[0], 0x03030303u);
    fill(nm, 60);
    answer_a(s, &fl, nm, 0x04040404u, 0, a);
    T_EQ(mt_dns_query(s, nm, MT_DNS_Q_A, &r, on_done, &g, 5), MT_DNS_IN_PROGRESS);
}

T_MAIN(test_dns2_abs_not_search, test_dns2_search_not_abs, test_dns2_domain_change,
       test_dns2_case_insensitive, test_dns2_clock_wrap, test_dns2_rejected_leaves_out,
       test_dns2_negative_retry_pending, test_dns2_cache_name_boundary)
