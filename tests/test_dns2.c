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

T_MAIN(test_dns2_abs_not_search, test_dns2_search_not_abs, test_dns2_domain_change,
       test_dns2_case_insensitive, test_dns2_clock_wrap, test_dns2_rejected_leaves_out)
