/* Plan 2 Task 11: the driver side of the DNR (mac/glue/mt_pb_dns.c), the private csCode
 * MT_CS_DNR that dnrp sends. Expected values are the real DNR's (matrix "DNR", ApiProbe group D):
 * rtnCode cacheFault while pending, cnames with a trailing dot, an IP literal has an empty cname,
 * noResultProc without a result proc, nameSyntaxErr, outOfMemory beyond the free slots. The
 * dnrp side (mac/dnr) is Retro68-only and is checked in Snow (scripts/probe-run.sh --groups D). */
#include "t.h"
#include "pbfake.h"
#include "dnsb.h"
#include "mt_util.h"

/* A DNR PB in fake memory with a recording completion routine. */
static MtDnrPb *dpb(int op, const char *name, uint32_t arg, MacPtr32 out, int hasproc) {
    MtDnrPb *p = (MtDnrPb *)pbf_alloc(sizeof(MtDnrPb));
    p->csCode = MT_CS_DNR;
    p->op = (int16_t)op;
    if (name) {
        char *n = (char *)pbf_alloc((uint32_t)strlen(name) + 1u);
        strcpy(n, name);
        p->name = pbf_a(n);
    }
    p->arg = arg;
    p->out = out;
    p->hasproc = (int16_t)hasproc;
    return p;
}
static void nop_compl(MtAnyPb *pb) { (void)pb; }

#define HI_INIT hi = host_info();
static uint8_t *host_info(void) { return (uint8_t *)pbf_alloc(280); }
static int32_t rtn_of(const uint8_t *hi) { return (int32_t)mt_rd32(hi); }
static const char *cname_of(const uint8_t *hi) { return (const char *)hi + 4; }

static void dns_init(pbt *x) {
    mt_ifconfig c;
    pbt_init(x, 1);
    mt_get_config(x->d->s, &c);
    c.dns = T_DNS;
    mt_pb_set_config(x->d, &c);
    mt_arp_insert(x->d->s, T_DNS, T_PEER_MAC, 0);
}

static int16_t call(pbt *x, MtDnrPb *p) {
    p->ioCompletion = pbf_compl(nop_compl);
    return pbt_call(x, (TCPiopb *)p);
}

/* The i-th frame the driver sent, as a DNS query. */
static void query_at(pbt *x, int i, qv *q) { get_q(&x->fl, i, q); }

static void answer_a(pkt *p, const qv *q, mt_ip4 a1, mt_ip4 a2) {
    begin(p, q->id, 0x8180, q->name, 1);
    count_an(p, a2 ? 2 : 1);
    rr_a(p, 300, a1);
    if (a2) rr_a(p, 300, a2);
}

static void answer_ptr(pkt *p, const qv *q, const char *target) {
    int at;
    begin(p, q->id, 0x8180, q->name, 12);
    count_an(p, 1);
    at = rr(p, 12, 600);
    pname(p, target);
    endrr(p, at);
}

static void a_lookup_pending_then_answer(void) {
    pbt x; uint8_t *hi; MtDnrPb *p; qv q; pkt a;
    dns_init(&x);
    HI_INIT
    memset(hi, 0x55, 280);
    p = dpb(MT_DNR_A, "host.example.com", 0, pbf_a(hi), 1);
    T_EQ(call(&x, p), inProgress);
    T_EQ(p->deferred, 1);
    T_CHECK(p->slot >= 0 && p->slot < MT_PB_DNS);
    T_EQ(rtn_of(hi), cacheFault);                                 /* matrix D12: from the start */
    T_EQ(pbf_count('C'), 0);
    query_at(&x, x.fl.n - 1, &q);
    T_CHECK(q.ok); T_EQ(q.qtype, 1); T_CHECK(strcmp(q.name, "host.example.com") == 0);
    answer_a(&a, &q, 0x0A000001u, 0x0A000002u);
    reply(x.d->s, &q, &a, pbf_now + 40);
    T_EQ(pbf_count('C'), 1);
    T_EQ(p->ioResult, 0);
    T_EQ(rtn_of(hi), 0);
    T_CHECK(strcmp(cname_of(hi), "host.example.com.") == 0);      /* the real DNR's trailing dot */
    T_EQ(mt_rd32(hi + 260), 0x0A000001u);
    T_EQ(mt_rd32(hi + 264), 0x0A000002u);
    T_EQ(mt_rd32(hi + 268), 0);
    T_EQ(mt_rd32(hi + 272), 0);
    T_EQ(pbf_bad, 0);
}

static void ip_literal_and_cache_hit_answer_at_once(void) {
    pbt x; uint8_t *hi; MtDnrPb *p; qv q; pkt a;
    dns_init(&x);
    HI_INIT
    p = dpb(MT_DNR_A, "10.1.2.3", 0, pbf_a(hi), 1);
    T_EQ(call(&x, p), 0);                                          /* matrix D10 */
    T_EQ(p->deferred, 0);
    T_EQ(rtn_of(hi), 0);
    T_EQ(hi[4], 0);                                                /* empty cname for a literal */
    T_EQ(mt_rd32(hi + 260), 0x0A010203u);
    /* a name answered once is answered at once the second time (matrix D13) */
    p = dpb(MT_DNR_A, "host.example.com", 0, pbf_a(hi), 1);
    T_EQ(call(&x, p), inProgress);
    query_at(&x, x.fl.n - 1, &q);
    answer_a(&a, &q, 0x0A000009u, 0);
    reply(x.d->s, &q, &a, pbf_now + 5);
    p = dpb(MT_DNR_A, "HOST.example.com", 0, pbf_a(hi), 1);
    memset(hi, 0, 280);
    T_EQ(call(&x, p), 0);
    T_EQ(p->deferred, 0);
    T_EQ(rtn_of(hi), 0);
    T_CHECK(strcmp(cname_of(hi), "host.example.com.") == 0);
    T_EQ(mt_rd32(hi + 260), 0x0A000009u);
}

static void errors_are_the_dnr_codes(void) {
    pbt x; uint8_t *hi; MtDnrPb *p; qv q; pkt a; int c0;
    dns_init(&x);
    HI_INIT
    p = dpb(MT_DNR_A, "bad name!.example", 0, pbf_a(hi), 1);
    T_EQ(call(&x, p), nameSyntaxErr);                              /* matrix D16 */
    T_EQ(rtn_of(hi), nameSyntaxErr);
    T_EQ(p->deferred, 0);
    c0 = pbf_count('C');
    /* NXDOMAIN: the PB completes 0, rtnCode carries authNameErr */
    p = dpb(MT_DNR_A, "nx.example.com", 0, pbf_a(hi), 1);
    T_EQ(call(&x, p), inProgress);
    query_at(&x, x.fl.n - 1, &q);
    begin(&a, q.id, 0x8183, q.name, 1);                            /* rcode 3 */
    reply(x.d->s, &q, &a, pbf_now + 20);
    T_EQ(pbf_count('C'), c0 + 1);
    T_EQ(p->ioResult, 0);
    T_EQ(rtn_of(hi), authNameErr);
    /* no answer data: noAnsErr */
    p = dpb(MT_DNR_A, "nodata.example.com", 0, pbf_a(hi), 1);
    T_EQ(call(&x, p), inProgress);
    query_at(&x, x.fl.n - 1, &q);
    begin(&a, q.id, 0x8180, q.name, 1);
    reply(x.d->s, &q, &a, pbf_now + 20);
    T_EQ(rtn_of(hi), noAnsErr);
    /* no reply at all: the core gives up, noNameServer */
    p = dpb(MT_DNR_A, "dead.example.com", 0, pbf_a(hi), 1);
    T_EQ(call(&x, p), inProgress);
    pbf_now += 60000;
    mt_pb_tick(x.d);
    T_EQ(p->ioResult, 0);
    T_EQ(rtn_of(hi), noNameServer);
    /* no server configured */
    {
        mt_ifconfig c;
        mt_get_config(x.d->s, &c);
        c.dns = 0;
        mt_pb_set_config(x.d, &c);
        p = dpb(MT_DNR_A, "x.example.com", 0, pbf_a(hi), 1);
        T_EQ(call(&x, p), noNameServer);
        T_EQ(rtn_of(hi), noNameServer);
    }
}

static void no_result_proc_asks_nothing(void) {
    pbt x; uint8_t *hi; MtDnrPb *p; qv q; pkt a;
    int n0;
    dns_init(&x);
    HI_INIT
    n0 = x.fl.n;
    p = dpb(MT_DNR_A, "later.example.com", 0, pbf_a(hi), 0);
    T_EQ(call(&x, p), noResultProc);                                /* matrix D20 */
    T_EQ(rtn_of(hi), noResultProc);
    T_EQ(p->deferred, 0);
    /* the query it started was cancelled: a late answer is ignored and writes nothing */
    query_at(&x, n0, &q);
    memset(hi, 0x77, 280);
    answer_a(&a, &q, 0x0A000001u, 0);
    reply(x.d->s, &q, &a, pbf_now + 5);
    T_EQ(hi[0], 0x77); T_EQ(hi[100], 0x77); T_EQ(hi[260], 0x77);
    /* and the slot was never held */
    p = dpb(MT_DNR_A, "later.example.com", 0, pbf_a(hi), 1);
    T_EQ(call(&x, p), inProgress);                                  /* a cancelled query caches nothing (matrix D21) */
}

static void slots_are_limited(void) {
    pbt x; uint8_t *hi[MT_PB_DNS + 1]; MtDnrPb *p[MT_PB_DNS + 1]; int i, c0;
    char nm[24];
    dns_init(&x);
    
    for (i = 0; i < MT_PB_DNS + 1; i++) {
        hi[i] = host_info();
        sprintf(nm, "h%d.example.com", i);
        p[i] = dpb(MT_DNR_A, nm, 0, pbf_a(hi[i]), 1);
        if (i < MT_PB_DNS) T_EQ(call(&x, p[i]), inProgress);
    }
    T_EQ(call(&x, p[MT_PB_DNS]), outOfMemory);                      /* matrix D31 (the real DNR has 3) */
    T_EQ(rtn_of(hi[MT_PB_DNS]), outOfMemory);
    c0 = pbf_count('C');
    pbf_now += 60000;
    mt_pb_tick(x.d);                                                /* all four time out: slots free */
    T_EQ(pbf_count('C'), c0 + MT_PB_DNS);
    p[MT_PB_DNS] = dpb(MT_DNR_A, "again.example.com", 0, pbf_a(hi[0]), 1);
    T_EQ(call(&x, p[MT_PB_DNS]), inProgress);
}

static void ptr_lookups_queue_behind_the_scratch(void) {
    pbt x; uint8_t *h1, *h2; MtDnrPb *p1, *p2; qv q; pkt a;
    int n;
    dns_init(&x);
    h1 = host_info(); h2 = host_info();
    p1 = dpb(MT_DNR_PTR, NULL, 0x0A000001u, pbf_a(h1), 1);
    T_EQ(call(&x, p1), inProgress);
    T_EQ(rtn_of(h1), cacheFault);
    n = x.fl.n;
    p2 = dpb(MT_DNR_PTR, NULL, 0x0A000002u, pbf_a(h2), 1);
    T_EQ(call(&x, p2), inProgress);                                 /* queued: core says BUSY */
    T_EQ(p2->deferred, 1);
    T_EQ(x.fl.n, n);                                                /* nothing asked yet */
    query_at(&x, n - 1, &q);
    T_EQ(q.qtype, 12); T_CHECK(strcmp(q.name, "1.0.0.10.in-addr.arpa") == 0);
    answer_ptr(&a, &q, "one.example.");
    reply(x.d->s, &q, &a, pbf_now + 20);
    T_EQ(pbf_count('C'), 1);                                        /* the first completed ... */
    T_EQ(rtn_of(h1), 0);
    T_CHECK(strcmp(cname_of(h1), "one.example.") == 0);
    T_EQ(mt_rd32(h1 + 260), 0);                                     /* addresses stay 0 (matrix D50) */
    T_EQ(x.fl.n, n + 1);                                            /* ... and the second one started */
    query_at(&x, n, &q);
    T_CHECK(strcmp(q.name, "2.0.0.10.in-addr.arpa") == 0);
    answer_ptr(&a, &q, "two.example");
    reply(x.d->s, &q, &a, pbf_now + 40);
    T_EQ(pbf_count('C'), 2);
    T_CHECK(strcmp(cname_of(h2), "two.example.") == 0);
    T_EQ(pbf_bad, 0);
}

static void cancel_completes_the_pb_and_stops_writes(void) {
    pbt x; uint8_t *hi; MtDnrPb *p, *c; qv q; pkt a; int c0;
    dns_init(&x);
    HI_INIT
    p = dpb(MT_DNR_A, "slow.example.com", 0, pbf_a(hi), 1);
    T_EQ(call(&x, p), inProgress);
    query_at(&x, x.fl.n - 1, &q);
    c = dpb(MT_DNR_CANCEL, NULL, 0, 0, 0);
    c->slot = p->slot;
    c->name = pbf_a(p);                                             /* the pending PB's address */
    T_EQ(call(&x, c), 0);
    T_EQ(p->ioResult, -27);                                         /* completed with abortErr */
    T_EQ(pbf_count('C'), 2);                                        /* the victim and the cancel PB */
    memset(hi, 0x66, 280);
    answer_a(&a, &q, 0x0A000001u, 0);
    reply(x.d->s, &q, &a, pbf_now + 5);
    T_EQ(hi[0], 0x66); T_EQ(hi[4], 0x66); T_EQ(hi[260], 0x66);
    /* a stale cancel (slot reused by another lookup, other PB address) changes nothing */
    p = dpb(MT_DNR_A, "other.example.com", 0, pbf_a(hi), 1);
    T_EQ(call(&x, p), inProgress);
    c = dpb(MT_DNR_CANCEL, NULL, 0, 0, 0);
    c->slot = p->slot;
    c->name = 0x1234;
    c0 = pbf_count('C');
    T_EQ(call(&x, c), 0);
    T_EQ(pbf_count('C'), c0 + 1);                                   /* only the cancel PB itself completed */
    T_EQ(p->deferred, 1);
}

static void goodbye_drops_pending_lookups_silently(void) {
    pbt x; uint8_t *hi; MtDnrPb *p; qv q; pkt a;
    int c0;
    dns_init(&x);
    HI_INIT
    p = dpb(MT_DNR_A, "bye.example.com", 0, pbf_a(hi), 1);
    T_EQ(call(&x, p), inProgress);
    query_at(&x, x.fl.n - 1, &q);
    c0 = pbf_count('C');
    mt_pb_goodbye(x.d);
    T_EQ(pbf_count('C'), c0);                                       /* no completion routine runs */
    memset(hi, 0x66, 280);
    answer_a(&a, &q, 0x0A000001u, 0);
    reply(x.d->s, &q, &a, pbf_now + 5);
    T_EQ(hi[0], 0x66); T_EQ(hi[260], 0x66);
    T_EQ(pbf_count('C'), c0);
    /* a lookup outside the departing application's memory survives */
    p = dpb(MT_DNR_A, "stay.example.com", 0, pbf_a(hi), 1);
    T_EQ(call(&x, p), inProgress);
    pbf_app_lo = 0x00500000u; pbf_app_hi = 0x00500001u;             /* the "application" is elsewhere */
    mt_pb_goodbye(x.d);
    pbf_app_lo = 0; pbf_app_hi = 0xFFFFFFFFu;
    query_at(&x, x.fl.n - 1, &q);
    answer_a(&a, &q, 0x0A000001u, 0);
    reply(x.d->s, &q, &a, pbf_now + 5);
    T_EQ(p->ioResult, 0);
    T_EQ(rtn_of(hi), 0);
}

static void enum_cache_lists_server_then_answers(void) {
    pbt x; uint8_t *hi; MtDnrPb *p; MtDnrEnum *e; qv q; pkt a; int i;
    dns_init(&x);
    HI_INIT
    for (i = 0; i < 2; i++) {
        p = dpb(MT_DNR_A, i ? "b.example.com" : "a.example.com", 0, pbf_a(hi), 1);
        T_EQ(call(&x, p), inProgress);
        query_at(&x, x.fl.n - 1, &q);
        answer_a(&a, &q, 0x0A000001u + (uint32_t)i, i ? 0x0A000010u : 0);
        reply(x.d->s, &q, &a, pbf_now + 5);
    }
    e = (MtDnrEnum *)pbf_alloc(sizeof(MtDnrEnum));
    p = dpb(MT_DNR_ENUM, NULL, 0, pbf_a(e), 0);
    T_EQ(call(&x, p), 0);
    T_EQ(p->slot, 1);                                               /* matrix D60: the server record first */
    T_EQ(e->type, 17); T_EQ(e->cclass, 1); T_EQ(e->rdata, T_DNS); T_EQ(e->ttl, 0xFFFFFFFFu);
    T_EQ(e->name[0], 0);
    for (i = 1; i <= 3; i++) {                                      /* a (1 address), b (2 addresses) */
        p = dpb(MT_DNR_ENUM, NULL, (uint32_t)i, pbf_a(e), 0);
        T_EQ(call(&x, p), 0);
        T_EQ(p->slot, 1);
        T_EQ(e->type, 1); T_EQ(e->cclass, 1);
        T_CHECK(e->ttl > 0 && e->ttl <= 301);
    }
    T_CHECK(strcmp(e->name, "b.example.com.") == 0);
    T_EQ(e->rdata, 0x0A000010u);
    p = dpb(MT_DNR_ENUM, NULL, 4, pbf_a(e), 0);
    T_EQ(call(&x, p), 0);
    T_EQ(p->slot, 0);                                               /* past the end */
}

static void unconfigured_is_not_open(void) {
    pbt x; uint8_t *hi; MtDnrPb *p;
    pbt_init(&x, 0);
    hi = host_info();
    p = dpb(MT_DNR_A, "10.1.2.3", 0, pbf_a(hi), 1);
    T_EQ(call(&x, p), -28);
    p = dpb(99, NULL, 0, 0, 0);
    T_EQ(call(&x, p), -28);
}

static void unknown_op_is_the_unimplemented_code(void) {
    pbt x; MtDnrPb *p;
    dns_init(&x);
    
    p = dpb(99, NULL, 0, 0, 0);
    T_EQ(call(&x, p), MT_PB_UNIMP);
    T_EQ(sizeof(MtDnrPb), 48);
    T_EQ(offsetof(MtDnrPb, op), 28);
    T_EQ(offsetof(MtDnrPb, deferred), 46);
}

T_MAIN(a_lookup_pending_then_answer, ip_literal_and_cache_hit_answer_at_once,
       errors_are_the_dnr_codes, no_result_proc_asks_nothing, slots_are_limited,
       ptr_lookups_queue_behind_the_scratch, cancel_completes_the_pb_and_stops_writes,
       goodbye_drops_pending_lookups_silently, enum_cache_lists_server_then_answers,
       unconfigured_is_not_open, unknown_op_is_the_unimplemented_code)
