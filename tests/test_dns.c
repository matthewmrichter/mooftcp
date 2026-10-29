#include "t.h"
#include "net.h"
#include "mt_int.h"
#include "mt_util.h"

#include "dnsb.h"

static void test_dns_dotted_quad(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    mt_dns_result r; got g = {0};
    T_EQ(mt_dns_query(s, "192.168.50.10", MT_DNS_Q_A, &r, on_done, &g, 0), 0);
    T_EQ(r.status, MT_DNS_OK); T_EQ(r.naddr, 1); T_EQ(r.addr[0], T_PEER);
    T_CHECK(strcmp(r.cname, "192.168.50.10") == 0);
    T_EQ(mt_dns_query(s, "0.0.0.0", MT_DNS_Q_A, &r, on_done, &g, 0), 0);
    T_EQ(r.addr[0], 0);
    T_EQ(mt_dns_query(s, "255.255.255.255", MT_DNS_Q_A, &r, on_done, &g, 0), 0);
    T_EQ(r.addr[0], 0xFFFFFFFFu);
    T_EQ(mt_dns_query(s, "10.1.2.3.", MT_DNS_Q_A, &r, on_done, &g, 0), 0);   /* trailing dot */
    T_EQ(r.addr[0], 0x0A010203u);
    T_EQ(fl.n, 0); T_EQ(g.n, 0);
    /* numeric but out of range: BAD_NAME; not exactly four numeric parts: hostnames */
    T_EQ(mt_dns_query(s, "999.1.1.1", MT_DNS_Q_A, &r, on_done, &g, 0), -MT_DNS_BAD_NAME);
    T_EQ(mt_dns_query(s, "1.2.3.1000", MT_DNS_Q_A, &r, on_done, &g, 0), -MT_DNS_BAD_NAME);
    T_EQ(fl.n, 0);
    T_EQ(mt_dns_query(s, "1.2.3", MT_DNS_Q_A, &r, on_done, &g, 0), MT_DNS_IN_PROGRESS);
    T_EQ(mt_dns_query(s, "1.2.3.4.5", MT_DNS_Q_A, &r, on_done, &g, 0), MT_DNS_IN_PROGRESS);
    T_EQ(mt_dns_query(s, "1e1.2.3.4", MT_DNS_Q_A, &r, on_done, &g, 0), MT_DNS_IN_PROGRESS);
    T_EQ(fl.n, 3);
    /* works with no server configured */
    s = tstack_new(&fl, T_ME);
    T_EQ(mt_dns_query(s, "1.2.3.4", MT_DNS_Q_A, &r, on_done, &g, 0), 0);
}

static void test_dns_a_query_and_answer(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    mt_dns_result r; got g = {0};
    qv q; pkt p; int at, tgt;
    T_EQ(mt_dns_query(s, "en.wikipedia.org", MT_DNS_Q_A, &r, on_done, &g, 1000), MT_DNS_IN_PROGRESS);
    T_EQ(fl.n, 1);
    get_q(&fl, 0, &q);
    T_CHECK(q.ok); T_EQ(q.dst, T_DNS); T_EQ(q.dport, 53); T_EQ(q.qtype, 1);
    T_CHECK(strcmp(q.name, "en.wikipedia.org") == 0);
    T_CHECK(q.sport >= 49152);
    /* CNAME en.wikipedia.org -> dyna.wikimedia.org; two A records for the target */
    begin(&p, q.id, 0x8180, "en.wikipedia.org", 1);
    count_an(&p, 3);
    at = rr(&p, 5, 600);
    tgt = pname(&p, "dyna.wikimedia.org");
    endrr(&p, at);
    pptr(&p, tgt); p16(&p, 1); p16(&p, 1); p32(&p, 300); p16(&p, 4); p32(&p, 0x0A000001u);
    pptr(&p, tgt); p16(&p, 1); p16(&p, 1); p32(&p, 200); p16(&p, 4); p32(&p, 0x0A000002u);
    reply(s, &q, &p, 1050);
    T_EQ(g.n, 1);
    T_EQ(g.r.status, MT_DNS_OK); T_EQ(g.r.naddr, 2);
    T_EQ(g.r.addr[0], 0x0A000001u); T_EQ(g.r.addr[1], 0x0A000002u);
    T_CHECK(strcmp(g.r.cname, "dyna.wikimedia.org") == 0);
    T_EQ(mt_get_stats(s)->udp_in, 1);
    T_EQ(mt_dns_deadline(s, 1050), MT_NO_DEADLINE);   /* nothing left pending */
}

static void test_dns_cache_hit(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    mt_dns_result r; got g = {0};
    qv q; pkt p;
    mt_dns_query(s, "Host.Example.com", MT_DNS_Q_A, &r, on_done, &g, 0);
    get_q(&fl, 0, &q);
    begin(&p, q.id, 0x8180, "host.example.com", 1);
    count_an(&p, 1);
    rr_a(&p, 10, 0x01020304u);                      /* TTL below the 60 s floor */
    reply(s, &q, &p, 5);
    T_EQ(g.n, 1);
    fake_clear(&fl);
    memset(&r, 0xAA, sizeof r);
    T_EQ(mt_dns_query(s, "host.example.com", MT_DNS_Q_A, &r, on_done, &g, 20000), 0);
    T_EQ(r.status, MT_DNS_OK); T_EQ(r.naddr, 1); T_EQ(r.addr[0], 0x01020304u);
    T_CHECK(strcmp(r.cname, "Host.Example.com") == 0);   /* canonical name as first resolved */
    T_EQ(fl.n, 0); T_EQ(g.n, 1);                    /* case-insensitive, no callback */
    T_EQ(mt_dns_query(s, "HOST.example.com", MT_DNS_Q_A, &r, on_done, &g, 5 + 59999), 0);
    /* expired after the 60 s floor: a network query again */
    T_EQ(mt_dns_query(s, "host.example.com", MT_DNS_Q_A, &r, on_done, &g, 5 + 60000),
         MT_DNS_IN_PROGRESS);
    T_EQ(fl.n, 1);
    /* TTL cap: 10 days becomes 24 h */
    get_q(&fl, 0, &q);
    begin(&p, q.id, 0x8180, "host.example.com", 1);
    count_an(&p, 1);
    rr_a(&p, 864000, 0x01020304u);
    reply(s, &q, &p, 100000);
    T_EQ(g.n, 2);
    T_EQ(mt_dns_query(s, "host.example.com", MT_DNS_Q_A, &r, on_done, &g, 100000 + 86399000u), 0);
    T_EQ(mt_dns_query(s, "host.example.com", MT_DNS_Q_A, &r, on_done, &g, 100000 + 86400000u),
         MT_DNS_IN_PROGRESS);
}

static void test_dns_domain_append(void) {
    fake_link fl; mt_stack *s = setup(&fl, "lan");
    mt_dns_result r; got g = {0};
    qv q; pkt p;
    T_EQ(mt_dns_query(s, "nas", MT_DNS_Q_A, &r, on_done, &g, 0), MT_DNS_IN_PROGRESS);
    get_q(&fl, 0, &q);
    T_CHECK(strcmp(q.name, "nas.lan") == 0);
    begin(&p, q.id, 0x8183, "nas.lan", 1);          /* NXDOMAIN */
    reply(s, &q, &p, 100);
    T_EQ(g.n, 0);
    T_EQ(fl.n, 2);
    get_q(&fl, 1, &q);
    T_CHECK(q.ok); T_CHECK(strcmp(q.name, "nas") == 0);
    begin(&p, q.id, 0x8180, "nas", 1);
    count_an(&p, 1);
    rr_a(&p, 300, T_PEER);
    reply(s, &q, &p, 200);
    T_EQ(g.n, 1); T_EQ(g.r.status, MT_DNS_OK); T_EQ(g.r.addr[0], T_PEER);
    /* a name with a dot is never extended; a trailing dot is absolute */
    fake_clear(&fl);
    mt_dns_query(s, "a.b", MT_DNS_Q_A, &r, on_done, &g, 300);
    mt_dns_query(s, "web.", MT_DNS_Q_A, &r, on_done, &g, 300);
    T_EQ(fl.n, 2);
    get_q(&fl, 0, &q); T_CHECK(strcmp(q.name, "a.b") == 0);
    get_q(&fl, 1, &q); T_CHECK(strcmp(q.name, "web") == 0);
}

static void test_dns_retry_timeout(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    mt_dns_result r; got g = {0};
    qv q0, q1;
    mt_dns_query(s, "slow.example", MT_DNS_Q_A, &r, on_done, &g, 1000);
    T_EQ(fl.n, 1);
    T_EQ(mt_next_deadline(s, 1000), 2000);
    mt_tick(s, 2999); T_EQ(fl.n, 1);
    T_EQ(mt_next_deadline(s, 2999), 1);
    mt_tick(s, 3000); T_EQ(fl.n, 2);
    get_q(&fl, 0, &q0); get_q(&fl, 1, &q1);
    T_CHECK(q1.ok); T_EQ(q1.id, q0.id); T_CHECK(strcmp(q1.name, "slow.example") == 0);
    T_EQ(mt_next_deadline(s, 3000), 4000);
    mt_tick(s, 6999); T_EQ(fl.n, 2);
    mt_tick(s, 7000); T_EQ(fl.n, 3);
    T_EQ(mt_next_deadline(s, 7000), 8000);
    mt_tick(s, 14999); T_EQ(fl.n, 3); T_EQ(g.n, 0);
    mt_tick(s, 15000);
    T_EQ(fl.n, 3); T_EQ(g.n, 1); T_EQ(g.r.status, MT_DNS_TIMEOUT);
    T_EQ(mt_dns_deadline(s, 15000), MT_NO_DEADLINE);
    mt_tick(s, 40000); T_EQ(g.n, 1);
    /* the slot is free again */
    T_EQ(mt_dns_query(s, "slow.example", MT_DNS_Q_A, &r, on_done, &g, 40000), MT_DNS_IN_PROGRESS);
}

static void test_dns_nxdomain(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    mt_dns_result r; got g = {0};
    qv q; pkt p;
    mt_dns_query(s, "nope.example", MT_DNS_Q_A, &r, on_done, &g, 0);
    get_q(&fl, 0, &q);
    begin(&p, q.id, 0x8183, "nope.example", 1);
    reply(s, &q, &p, 10);
    T_EQ(g.n, 1); T_EQ(g.r.status, MT_DNS_NOT_FOUND); T_EQ(g.r.naddr, 0);
    /* no matching records -> NO_ANSWER (also: an answer for another type only) */
    mt_dns_query(s, "empty.example", MT_DNS_Q_A, &r, on_done, &g, 20);
    get_q(&fl, 1, &q);
    begin(&p, q.id, 0x8180, "empty.example", 1);
    reply(s, &q, &p, 30);
    T_EQ(g.n, 2); T_EQ(g.r.status, MT_DNS_NO_ANSWER);
    mt_dns_query(s, "txt.example", MT_DNS_Q_A, &r, on_done, &g, 40);
    get_q(&fl, 2, &q);
    begin(&p, q.id, 0x8180, "txt.example", 1);
    count_an(&p, 1);
    { int at = rr(&p, 16, 60); p8(&p, 2); p8(&p, 'h'); p8(&p, 'i'); endrr(&p, at); }
    reply(s, &q, &p, 50);
    T_EQ(g.n, 3); T_EQ(g.r.status, MT_DNS_NO_ANSWER);
    /* SERVFAIL completes as NO_ANSWER */
    mt_dns_query(s, "sf.example", MT_DNS_Q_A, &r, on_done, &g, 60);
    get_q(&fl, 3, &q);
    begin(&p, q.id, 0x8182, "sf.example", 1);
    reply(s, &q, &p, 70);
    T_EQ(g.n, 4); T_EQ(g.r.status, MT_DNS_NO_ANSWER);
}

static void test_dns_no_server(void) {
    fake_link fl; mt_stack *s = tstack_new(&fl, T_ME);
    mt_dns_result r; got g = {0};
    T_EQ(mt_dns_query(s, "x.example", MT_DNS_Q_A, &r, on_done, &g, 0), -MT_DNS_NO_SERVER);
    T_EQ(fl.n, 0);
    /* no address at all: the send fails, also NO_SERVER, and the slot is not leaked */
    s = tstack_new(&fl, 0);
    { mt_ifconfig c; mt_get_config(s, &c); c.dns = T_DNS; mt_set_config(s, &c); }
    T_EQ(mt_dns_query(s, "x.example", MT_DNS_Q_A, &r, on_done, &g, 0), -MT_DNS_NO_SERVER);
    T_EQ(mt_dns_deadline(s, 0), MT_NO_DEADLINE);
}

static void test_dns_bad_name(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    mt_dns_result r; got g = {0};
    char big[300], lab[100];
    int i;
    T_EQ(mt_dns_query(s, "", MT_DNS_Q_A, &r, on_done, &g, 0), -MT_DNS_BAD_NAME);
    T_EQ(mt_dns_query(s, ".", MT_DNS_Q_A, &r, on_done, &g, 0), -MT_DNS_BAD_NAME);
    T_EQ(mt_dns_query(s, ".a.b", MT_DNS_Q_A, &r, on_done, &g, 0), -MT_DNS_BAD_NAME);
    T_EQ(mt_dns_query(s, "a..b", MT_DNS_Q_A, &r, on_done, &g, 0), -MT_DNS_BAD_NAME);
    T_EQ(mt_dns_query(s, "a b.c", MT_DNS_Q_A, &r, on_done, &g, 0), -MT_DNS_BAD_NAME);
    memset(lab, 'a', 64); lab[64] = 0;
    T_EQ(mt_dns_query(s, lab, MT_DNS_Q_A, &r, on_done, &g, 0), -MT_DNS_BAD_NAME);    /* label 64 */
    memset(big, 'a', 299); big[299] = 0;
    T_EQ(mt_dns_query(s, big, MT_DNS_Q_A, &r, on_done, &g, 0), -MT_DNS_BAD_NAME);
    for (i = 0; i < 64; i++) strcpy(big + i * 4, "abc.");
    big[255] = 0;
    T_EQ(mt_dns_query(s, big, MT_DNS_Q_A, &r, on_done, &g, 0), -MT_DNS_BAD_NAME);    /* 255 chars */
    big[253] = 0;                                    /* 253 chars, "abc.abc..abc" ends in '.', valid labels */
    big[252] = 'c';
    T_EQ(mt_dns_query(s, big, MT_DNS_Q_A, &r, on_done, &g, 0), MT_DNS_IN_PROGRESS);
    memset(lab, 'a', 63); lab[63] = 0;
    T_EQ(mt_dns_query(s, lab, MT_DNS_Q_A, &r, on_done, &g, 0), MT_DNS_IN_PROGRESS);  /* label 63 */
    T_EQ(mt_dns_query(s, NULL, MT_DNS_Q_A, &r, on_done, &g, 0), -MT_DNS_BAD_NAME);
    T_EQ(fl.n, 2);
}

static void test_dns_ptr(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    mt_dns_result r; got g = {0};
    char nm[32];
    qv q; pkt p; int at;
    T_EQ(mt_dns_ptr_name(T_PEER, nm), 26);
    T_CHECK(strcmp(nm, "10.50.168.192.in-addr.arpa") == 0);
    T_EQ(mt_dns_ptr_name(0xFFFFFFFFu, nm), 28);
    T_CHECK(strcmp(nm, "255.255.255.255.in-addr.arpa") == 0);
    mt_dns_ptr_name(T_PEER, nm);
    T_EQ(mt_dns_query(s, nm, MT_DNS_Q_PTR, &r, on_done, &g, 0), MT_DNS_IN_PROGRESS);
    get_q(&fl, 0, &q);
    T_EQ(q.qtype, 12); T_CHECK(strcmp(q.name, nm) == 0);
    begin(&p, q.id, 0x8180, nm, 12);
    count_an(&p, 1);
    at = rr(&p, 12, 3600);
    pname(&p, "printer.lan");
    endrr(&p, at);
    reply(s, &q, &p, 10);
    T_EQ(g.n, 1); T_EQ(g.r.status, MT_DNS_OK);
    T_CHECK(strcmp(g.r.cname, "printer.lan") == 0);
}

static void test_dns_hinfo_mx(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    mt_dns_result r; got g = {0};
    qv q; pkt p; int at;
    mt_dns_query(s, "box.example", MT_DNS_Q_HINFO, &r, on_done, &g, 0);
    get_q(&fl, 0, &q);
    T_EQ(q.qtype, 13);
    begin(&p, q.id, 0x8180, "box.example", 13);
    count_an(&p, 1);
    at = rr(&p, 13, 60);
    p8(&p, 6); memcpy(p.b + p.n, "M68000", 6); p.n += 6;
    p8(&p, 7); memcpy(p.b + p.n, "System6", 7); p.n += 7;
    endrr(&p, at);
    reply(s, &q, &p, 10);
    T_EQ(g.n, 1); T_EQ(g.r.status, MT_DNS_OK);
    T_CHECK(strcmp(g.r.cpu, "M68000") == 0); T_CHECK(strcmp(g.r.os, "System6") == 0);

    mt_dns_query(s, "example.org", MT_DNS_Q_MX, &r, on_done, &g, 20);
    get_q(&fl, 1, &q);
    T_EQ(q.qtype, 15);
    begin(&p, q.id, 0x8180, "example.org", 15);
    count_an(&p, 2);
    at = rr(&p, 15, 300); p16(&p, 20); pname(&p, "mx2.example.org"); endrr(&p, at);
    at = rr(&p, 15, 300); p16(&p, 10); pname(&p, "mx1.example.org"); endrr(&p, at);
    reply(s, &q, &p, 30);
    T_EQ(g.n, 2); T_EQ(g.r.status, MT_DNS_OK);
    T_EQ(g.r.mx_pref, 10); T_CHECK(strcmp(g.r.mx_name, "mx1.example.org") == 0);
}

static void test_dns_malformed_ignored(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    mt_dns_result r; got g = {0};
    qv q; pkt p; int at;
    mt_dns_query(s, "evil.example", MT_DNS_Q_A, &r, on_done, &g, 0);
    get_q(&fl, 0, &q);
    /* wrong ID */
    begin(&p, (uint16_t)(q.id + 1), 0x8180, "evil.example", 1);
    count_an(&p, 1); rr_a(&p, 60, 0x01010101u);
    reply(s, &q, &p, 10);
    /* wrong source address / port */
    begin(&p, q.id, 0x8180, "evil.example", 1);
    count_an(&p, 1); rr_a(&p, 60, 0x01010101u);
    deliver(s, &q, T_PEER, 53, &p, 20);
    deliver(s, &q, T_DNS, 5353, &p, 30);
    /* not a response / opcode != 0 */
    p.b[2] = 0x01;
    reply(s, &q, &p, 40);
    p.b[2] = 0xA1;
    reply(s, &q, &p, 41);
    /* question mismatch: name, type, class */
    begin(&p, q.id, 0x8180, "other.example", 1);
    count_an(&p, 1); rr_a(&p, 60, 0x01010101u);
    reply(s, &q, &p, 50);
    begin(&p, q.id, 0x8180, "evil.example", 28);
    count_an(&p, 1); rr_a(&p, 60, 0x01010101u);
    reply(s, &q, &p, 51);
    begin(&p, q.id, 0x8180, "evil.example", 1);
    count_an(&p, 1); rr_a(&p, 60, 0x01010101u);
    p.b[12 + 14 + 3] = 3;                            /* qclass CH */
    reply(s, &q, &p, 52);
    /* compression pointer to itself / forward / mutual loop / past the end */
    begin(&p, q.id, 0x8180, "evil.example", 1);
    count_an(&p, 1);
    at = p.n; pptr(&p, at); p16(&p, 1); p16(&p, 1); p32(&p, 60); p16(&p, 4); p32(&p, 7);
    reply(s, &q, &p, 60);
    begin(&p, q.id, 0x8180, "evil.example", 1);
    count_an(&p, 1);
    at = p.n; pptr(&p, at + 2); pptr(&p, at); p16(&p, 1); p16(&p, 1); p32(&p, 60); p16(&p, 4); p32(&p, 7);
    reply(s, &q, &p, 61);
    begin(&p, q.id, 0x8180, "evil.example", 1);
    count_an(&p, 1);
    pptr(&p, 0x3FFF); p16(&p, 1); p16(&p, 1); p32(&p, 60); p16(&p, 4); p32(&p, 7);
    reply(s, &q, &p, 62);
    /* CNAME whose target is a pointer loop */
    begin(&p, q.id, 0x8180, "evil.example", 1);
    count_an(&p, 1);
    at = rr(&p, 5, 60); pptr(&p, p.n); endrr(&p, at);
    reply(s, &q, &p, 63);
    /* reserved label type 0x40 */
    begin(&p, q.id, 0x8180, "evil.example", 1);
    count_an(&p, 1);
    at = rr(&p, 5, 60); p8(&p, 0x41); p8(&p, 'a'); p8(&p, 0); endrr(&p, at);
    reply(s, &q, &p, 64);
    /* truncated answers: rdlength past the end, header cut, RR cut, count too large */
    begin(&p, q.id, 0x8180, "evil.example", 1);
    count_an(&p, 1); rr_a(&p, 60, 0x01010101u);
    p.n -= 2;
    reply(s, &q, &p, 70);
    begin(&p, q.id, 0x8180, "evil.example", 1);
    count_an(&p, 1); rr_a(&p, 60, 0x01010101u);
    p.n -= 8;
    reply(s, &q, &p, 71);
    begin(&p, q.id, 0x8180, "evil.example", 1);
    count_an(&p, 5); rr_a(&p, 60, 0x01010101u);
    reply(s, &q, &p, 72);
    begin(&p, q.id, 0x8180, "evil.example", 1);
    p.n = 11;
    reply(s, &q, &p, 73);
    begin(&p, q.id, 0x8180, "evil.example", 1);
    p.n = 20;
    reply(s, &q, &p, 74);
    T_EQ(g.n, 0);
    T_EQ(fl.n, 1);
    mt_tick(s, 2000); T_EQ(fl.n, 2);                 /* the query goes on retrying */
    mt_tick(s, 14000);
    T_EQ(g.n, 1); T_EQ(g.r.status, MT_DNS_TIMEOUT);
    /* a late good answer for the dead query does nothing */
    begin(&p, q.id, 0x8180, "evil.example", 1);
    count_an(&p, 1); rr_a(&p, 60, 0x01010101u);
    reply(s, &q, &p, 14001);
    T_EQ(g.n, 1);
}

static void test_dns_tc_and_oversize(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    mt_dns_result r; got g = {0};
    qv q; pkt p;
    /* TC with one good A record: the records that are there are used */
    mt_dns_query(s, "big.example", MT_DNS_Q_A, &r, on_done, &g, 0);
    get_q(&fl, 0, &q);
    begin(&p, q.id, 0x8380, "big.example", 1);
    count_an(&p, 3); rr_a(&p, 60, 0x05050505u);      /* announces 3, delivers 1 */
    reply(s, &q, &p, 10);
    T_EQ(g.n, 1); T_EQ(g.r.status, MT_DNS_OK); T_EQ(g.r.naddr, 1); T_EQ(g.r.addr[0], 0x05050505u);
    /* TC with nothing usable: NO_ANSWER right away (there is no TCP fallback) */
    mt_dns_query(s, "big2.example", MT_DNS_Q_A, &r, on_done, &g, 20);
    get_q(&fl, 1, &q);
    begin(&p, q.id, 0x8380, "big2.example", 1);
    reply(s, &q, &p, 30);
    T_EQ(g.n, 2); T_EQ(g.r.status, MT_DNS_NO_ANSWER);
    /* more than 4 A records: the first four are kept */
    mt_dns_query(s, "many.example", MT_DNS_Q_A, &r, on_done, &g, 40);
    get_q(&fl, 2, &q);
    begin(&p, q.id, 0x8180, "many.example", 1);
    count_an(&p, 6);
    { int i; for (i = 0; i < 6; i++) rr_a(&p, 60, 0x0A000000u + (uint32_t)i); }
    reply(s, &q, &p, 50);
    T_EQ(g.n, 3); T_EQ(g.r.naddr, 4); T_EQ(g.r.addr[3], 0x0A000003u);
}

static void test_dns_concurrency_limit(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    mt_dns_result r[5]; got g[5]; qv q; pkt p;
    uint16_t ids[4];
    int i;
    char nm[16];
    memset(g, 0, sizeof g);
    for (i = 0; i < 4; i++) {
        sprintf(nm, "h%d.example", i);
        T_EQ(mt_dns_query(s, nm, MT_DNS_Q_A, &r[i], on_done, &g[i], 0), MT_DNS_IN_PROGRESS);
    }
    T_EQ(mt_dns_query(s, "h4.example", MT_DNS_Q_A, &r[4], on_done, &g[4], 0), -MT_DNS_BUSY);
    T_EQ(fl.n, 4);
    for (i = 0; i < 4; i++) { get_q(&fl, i, &q); ids[i] = q.id; }
    for (i = 0; i < 4; i++) { int j; for (j = i + 1; j < 4; j++) T_CHECK(ids[i] != ids[j]); }
    /* answer them out of order; each completes its own */
    for (i = 3; i >= 0; i--) {
        get_q(&fl, i, &q);
        sprintf(nm, "h%d.example", i);
        begin(&p, q.id, 0x8180, nm, 1);
        count_an(&p, 1); rr_a(&p, 60, 0x0A000000u + (uint32_t)i);
        reply(s, &q, &p, 10);
        T_EQ(g[i].n, 1); T_EQ(g[i].r.addr[0], 0x0A000000u + (uint32_t)i);
    }
    T_EQ(mt_dns_query(s, "h4.example", MT_DNS_Q_A, &r[4], on_done, &g[4], 20), MT_DNS_IN_PROGRESS);
}

typedef struct { int n; char name[4][64]; uint32_t ttl[4]; mt_ip4 a0[4]; uint8_t na[4]; } en;
static void on_enum(void *user, const char *name, const mt_ip4 *addr, uint8_t naddr, uint32_t ttl) {
    en *e = (en *)user;
    if (e->n < 4) {
        strcpy(e->name[e->n], name); e->ttl[e->n] = ttl; e->a0[e->n] = addr[0]; e->na[e->n] = naddr;
    }
    e->n++;
}
static void resolve(mt_stack *s, fake_link *fl, const char *name, mt_ip4 ip, uint32_t ttl,
                    uint32_t now) {
    mt_dns_result r; got g = {0}; qv q; pkt p;
    int idx = fl->n;
    T_EQ(mt_dns_query(s, name, MT_DNS_Q_A, &r, on_done, &g, now), MT_DNS_IN_PROGRESS);
    get_q(fl, idx, &q);
    begin(&p, q.id, 0x8180, name, 1);
    count_an(&p, 1); rr_a(&p, ttl, ip);
    reply(s, &q, &p, now);
    T_EQ(g.n, 1);
}

static void test_dns_enum_cache(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    en e; char long_name[80];
    memset(&e, 0, sizeof e);
    mt_dns_enum_cache(s, on_enum, &e, 0);
    T_EQ(e.n, 0);
    resolve(s, &fl, "one.example", 0x0A000001u, 300, 1000);
    resolve(s, &fl, "two.example", 0x0A000002u, 100, 1000);   /* floor 60 does not apply */
    /* a 64+ character name resolves but is neither cached nor listed */
    memset(long_name, 'a', 40); strcpy(long_name + 40, ".bbbbbbbbbbbbbbbbbbbbbbbbbbbb.com");
    resolve(s, &fl, long_name, 0x0A000003u, 300, 1000);
    mt_dns_enum_cache(s, on_enum, &e, 21000);
    T_EQ(e.n, 2);
    T_CHECK(strcmp(e.name[0], "one.example") == 0); T_EQ(e.ttl[0], 280); T_EQ(e.a0[0], 0x0A000001u);
    T_CHECK(strcmp(e.name[1], "two.example") == 0); T_EQ(e.ttl[1], 80); T_EQ(e.na[1], 1);
    memset(&e, 0, sizeof e);
    mt_dns_enum_cache(s, on_enum, &e, 1000 + 100000);   /* "two" has expired */
    T_EQ(e.n, 1);
    T_CHECK(strcmp(e.name[0], "one.example") == 0); T_EQ(e.ttl[0], 200);
    T_EQ(mt_dns_query(s, long_name, MT_DNS_Q_A, &(mt_dns_result){0}, on_done, &(got){0}, 5000),
         MT_DNS_IN_PROGRESS);
}

static void test_dns_cache_lru_and_endpoint(void) {
    fake_link fl; mt_stack *s = setup(&fl, NULL);
    mt_dns_result r;
    char nm[16];
    static uint8_t ubufs[8][1024];
    int i, base;
    /* the endpoint is created lazily and kept, in the reserved slot (not one of max_udp) */
    T_EQ(s->lim.max_udp, 8);
    for (i = 0; i < 7; i++) T_CHECK(mt_udp_create(s, (uint16_t)(2000 + i), ubufs[i], 1024, NULL, NULL) != NULL);
    for (i = 0; i < 8; i++) { sprintf(nm, "n%d.ex", i); resolve(s, &fl, nm, 0x0A000000u + (uint32_t)i, 1000, 100u + (uint32_t)i); }
    /* touch n0 so n1 is the least recently used */
    T_EQ(mt_dns_query(s, "n0.ex", MT_DNS_Q_A, &r, NULL, NULL, 500), 0);
    base = fl.n;
    resolve(s, &fl, "n8.ex", 0x0A000008u, 1000, 600);          /* evicts n1 */
    T_EQ(mt_dns_query(s, "n0.ex", MT_DNS_Q_A, &r, NULL, NULL, 700), 0);
    T_EQ(mt_dns_query(s, "n8.ex", MT_DNS_Q_A, &r, NULL, NULL, 700), 0);
    T_EQ(fl.n, base + 1);
    T_EQ(mt_dns_query(s, "n1.ex", MT_DNS_Q_A, &r, NULL, NULL, 700), MT_DNS_IN_PROGRESS);
    /* a stack whose app slots are all taken still resolves: the endpoint is a reserved extra slot */
    {
        fake_link fl2; mt_stack *s2 = setup(&fl2, NULL);
        static uint8_t bufs[8][1024];
        mt_dns_result r2;
        for (i = 0; i < 8; i++) T_CHECK(mt_udp_create(s2, (uint16_t)(3000 + i), bufs[i], 1024, NULL, NULL) != NULL);
        T_EQ(mt_dns_query(s2, "x.example", MT_DNS_Q_A, &r2, NULL, NULL, 0), MT_DNS_IN_PROGRESS);
        T_EQ(fl2.n, 1);
    }
}

T_MAIN(test_dns_dotted_quad, test_dns_a_query_and_answer, test_dns_cache_hit,
       test_dns_domain_append, test_dns_retry_timeout, test_dns_nxdomain, test_dns_no_server,
       test_dns_bad_name, test_dns_ptr, test_dns_hinfo_mx, test_dns_malformed_ignored,
       test_dns_tc_and_oversize, test_dns_concurrency_limit, test_dns_enum_cache,
       test_dns_cache_lru_and_endpoint)
