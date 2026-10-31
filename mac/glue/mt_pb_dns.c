/* mt_pb_dns.c - the driver side of the DNR: the private csCode MT_CS_DNR that dnrp (mac/dnr) sends
 * (mt_dnr_abi.h has the PB). One lookup = one MtDnsSlot holding the core's result memory in the
 * resident block; the answer is copied to the caller's hostInfo / returnRec only while the PB is
 * pending, and the goodbye kiss cancels lookups whose PB lies in the departing application.
 *
 * Results follow the real DNR (matrix "DNR", ApiProbe group D on MacTCP 2.1): rtnCode is cacheFault
 * from the moment a lookup is accepted, cnames carry the trailing dot, an IP literal has an empty
 * cname, a lookup without a result proc that is not answered at once is noResultProc.
 *
 * mt_dns_ptr serves one reverse lookup at a time (-MT_DNS_BUSY otherwise): a second reverse lookup
 * waits in its slot (state 2) and is started when the first one ends or is cancelled. */
#include <string.h>
#include "mt_pb.h"
#include "mt_util.h"

#define NAME_MAX_OUT 254                 /* hostInfo.cname is 255 bytes: 254 characters + NUL */
#define ABORT_ERR    (-27)               /* abortErr: a cancelled lookup's PB completes with it */

/* mt_dns_status -> DNR error (matrix: NXDOMAIN is authNameErr, no data noAnsErr, a dead server
 * noNameServer, bad syntax nameSyntaxErr). */
static int16_t dns_err(mt_dns_status st) {
    switch (st) {
    case MT_DNS_OK:        return 0;
    case MT_DNS_NOT_FOUND: return authNameErr;
    case MT_DNS_NO_ANSWER: return noAnsErr;
    case MT_DNS_TIMEOUT:
    case MT_DNS_NO_SERVER: return noNameServer;
    case MT_DNS_BUSY:      return outOfMemory;
    case MT_DNS_BAD_NAME:  return nameSyntaxErr;
    default:               return dnrErr;
    }
}

/* Exactly four numeric parts: the real DNR gives such a name an empty cname. */
static int is_quad(const char *s) {
    int parts = 1, digits = 0;
    for (; *s; s++) {
        if (*s == '.') { if (!digits) return 0; parts++; digits = 0; }
        else if (*s >= '0' && *s <= '9') digits++;
        else return 0;
    }
    return parts == 4 && digits > 0;
}

/* ---- writing into the caller's memory (byte-wise: the record may sit at any even address) ---- */

static void put_rtn(uint8_t *o, int32_t v) { mt_wr32(o, (uint32_t)v); }

/* cname[255] at offset 4: the name, a trailing dot (as the real DNR's), NUL. */
static void put_name(uint8_t *o, const char *name) {
    size_t n = strlen(name);
    if (n > NAME_MAX_OUT - 1u) n = NAME_MAX_OUT - 1u;
    memcpy(o + 4, name, n);
    if (n && name[n - 1] != '.') o[4 + n++] = '.';
    o[4 + n] = 0;
}

static void put_str(uint8_t *dst, const char *s, size_t cap) {   /* NUL-terminated copy */
    size_t n = strlen(s);
    if (n > cap - 1u) n = cap - 1u;
    memcpy(dst, s, n);
    dst[n] = 0;
}

static void deliver(const MtDnsSlot *q, mt_dns_status st) {
    uint8_t *o = (uint8_t *)mt_os_ptr(q->out);
    uint32_t i;
    if (!o) return;
    if (st != MT_DNS_OK) { put_rtn(o, dns_err(st)); return; }
    switch (q->op) {
    case MT_DNR_A:
        if (is_quad(q->res.cname)) o[4] = 0; else put_name(o, q->res.cname);
        for (i = 0; i < MT_DNS_MAXADDR; i++) mt_wr32(o + 260 + 4 * i, i < q->res.naddr ? q->res.addr[i] : 0);
        break;
    case MT_DNR_PTR:                                   /* the real DNR leaves the addresses 0 */
        put_name(o, q->res.cname);
        for (i = 0; i < MT_DNS_MAXADDR; i++) mt_wr32(o + 260 + 4 * i, 0);
        break;
    case MT_DNR_HINFO:                                 /* returnRec: cpuType[30], osType[30] at 260 */
        put_name(o, q->res.cname);
        put_str(o + MT_RETREC_UNION, q->res.cpu, 30);
        put_str(o + MT_RETREC_UNION + 30, q->res.os, 30);
        break;
    default:                                           /* MT_DNR_MX: preference, exchange[255] */
        put_name(o, q->res.cname);
        mt_wr16(o + MT_RETREC_UNION, q->res.mx_pref);
        put_name(o + MT_RETREC_UNION - 2, q->res.mx_name);   /* writes at +2 */
        break;
    }
    put_rtn(o, 0);
}

/* ---- starting, finishing ---- */

static void kick(MtDrv *d);

/* 0 = answered (q->res filled), MT_DNS_IN_PROGRESS, or -status. */
static int launch(MtDrv *d, MtDnsSlot *q, const char *name);
static void dns_cb(void *user, const mt_dns_result *r);
static void ptr_cb(void *user, mt_dns_status st, const char *name);

static int launch(MtDrv *d, MtDnsSlot *q, const char *name) {
    switch (q->op) {
    case MT_DNR_PTR:
        return mt_dns_ptr(d->s, q->addr, q->res.cname, (uint16_t)sizeof q->res.cname, ptr_cb, q, d->now);
    case MT_DNR_HINFO:
        return mt_dns_query(d->s, name, MT_DNS_Q_HINFO, &q->res, dns_cb, q, d->now);
    case MT_DNR_MX:
        return mt_dns_query(d->s, name, MT_DNS_Q_MX, &q->res, dns_cb, q, d->now);
    default:
        return mt_dns_query(d->s, name, MT_DNS_Q_A, &q->res, dns_cb, q, d->now);
    }
}

static void cancel_core(MtDrv *d, MtDnsSlot *q) {
    mt_dns_cancel(d->s, &q->res);
    mt_dns_cancel(d->s, q->res.cname);
}

/* A lookup ended (from the core, from kick). The slot is free before the app's routine runs, so
 * that routine can start the next lookup in it. */
static void finish(MtDnsSlot *q, mt_dns_status st) {
    MtDrv *d = q->d;
    MtAnyPb *pb = q->pb;
    if (!q->state) return;
    if (pb) deliver(q, st);
    q->state = 0;
    q->pb = NULL;
    if (pb) mt_pb_call_complete(d, pb, 0);
    kick(d);
}

static void dns_cb(void *user, const mt_dns_result *r) { finish((MtDnsSlot *)user, r->status); }
static void ptr_cb(void *user, mt_dns_status st, const char *name) { (void)name; finish((MtDnsSlot *)user, st); }

/* Start the reverse lookups that waited for the scratch buffer, oldest slot first. */
static void kick(MtDrv *d) {
    uint32_t i;
    for (i = 0; i < MT_PB_DNS; i++) {
        MtDnsSlot *q = &d->dns[i];
        int r;
        if (q->state != 2) continue;
        r = launch(d, q, NULL);
        if (r == MT_DNS_IN_PROGRESS) { q->state = 1; return; }     /* it holds the scratch now */
        if (r == -MT_DNS_BUSY) return;
        finish(q, r == 0 ? MT_DNS_OK : (mt_dns_status)-r);
        return;                                                    /* finish() kicked the rest */
    }
}

static int16_t lookup(MtDrv *d, MtAnyPb *pb) {
    MtDnrPb *p = &pb->dnr;
    uint8_t *o = (uint8_t *)mt_os_ptr(p->out);
    const char *name = p->op == MT_DNR_PTR ? NULL : (const char *)mt_os_ptr(p->name);
    MtDnsSlot *q = NULL;
    uint32_t i;
    int r;
    int16_t e;
    p->deferred = 0;
    if (!o || (p->op != MT_DNR_PTR && !name)) return invalidBufPtr;
    for (i = 0; i < MT_PB_DNS; i++)
        if (!d->dns[i].state) { q = &d->dns[i]; break; }
    if (!q) { put_rtn(o, outOfMemory); return outOfMemory; }
    q->d = d;
    q->pb = NULL;
    q->op = (uint8_t)p->op;
    q->out = p->out;
    q->addr = p->arg;
    put_rtn(o, cacheFault);                              /* from the moment the lookup is accepted */
    r = launch(d, q, name);
    if (r == 0) { deliver(q, MT_DNS_OK); return 0; }     /* answered at once: no result proc call */
    if (r == MT_DNS_IN_PROGRESS || (r == -MT_DNS_BUSY && p->op == MT_DNR_PTR)) {
        if (!p->hasproc) {                               /* matrix D20: noResultProc, nothing is asked */
            if (r == MT_DNS_IN_PROGRESS) cancel_core(d, q);
            put_rtn(o, noResultProc);
            return noResultProc;
        }
        q->state = (uint8_t)(r == MT_DNS_IN_PROGRESS ? 1 : 2);
        q->pb = pb;
        p->slot = (int16_t)i;
        p->deferred = 1;                                 /* before inProgress: the PB is the glue's now */
        return inProgress;
    }
    e = dns_err((mt_dns_status)-r);
    put_rtn(o, e);
    return e;
}

static int16_t do_cancel(MtDrv *d, MtAnyPb *pb) {
    MtDnrPb *p = &pb->dnr;
    MtDnsSlot *q;
    MtAnyPb *victim;
    if (p->slot < 0 || p->slot >= MT_PB_DNS) return 0;
    q = &d->dns[p->slot];
    if (!q->state || !q->pb || mt_os_addr(q->pb) != p->name) return 0;   /* name = the pending PB's address */
    victim = q->pb;
    cancel_core(d, q);
    q->state = 0;
    q->pb = NULL;
    mt_pb_call_complete(d, victim, ABORT_ERR);
    kick(d);
    return 0;
}

/* ---- EnumCache ---- */

typedef struct { uint32_t want, seen; MtDnrEnum *e; int found; } EnumCtx;

static void enum_cb(void *user, const char *name, const mt_ip4 *addr, uint8_t naddr, uint32_t ttl_left_s) {
    EnumCtx *c = (EnumCtx *)user;
    uint8_t i;
    for (i = 0; i < naddr; i++) {
        if (c->seen++ != c->want) continue;
        put_str((uint8_t *)c->e->name, name, 65);
        {
            size_t n = strlen(c->e->name);
            if (n && c->e->name[n - 1] != '.') { c->e->name[n] = '.'; c->e->name[n + 1] = 0; }
        }
        c->e->type = 1;
        c->e->cclass = 1;
        c->e->rdata = addr[i];
        c->e->ttl = ttl_left_s;
        c->found = 1;
    }
}

static int16_t do_enum(MtDrv *d, MtAnyPb *pb) {
    MtDnrPb *p = &pb->dnr;
    MtDnrEnum *e = (MtDnrEnum *)mt_os_ptr(p->out);
    mt_ifconfig c;
    EnumCtx ctx;
    uint32_t idx = p->arg;
    p->slot = 0;
    if (!e) return invalidBufPtr;
    mt_get_config(d->s, &c);
    if (c.dns) {
        if (idx == 0) {                                  /* the name server record comes first (matrix D60) */
            e->name[0] = 0;
            e->type = 17;
            e->cclass = 1;
            e->rdata = c.dns;
            e->ttl = 0xFFFFFFFFu;
            p->slot = 1;
            return 0;
        }
        idx--;
    }
    ctx.want = idx; ctx.seen = 0; ctx.e = e; ctx.found = 0;
    mt_dns_enum_cache(d->s, enum_cb, &ctx, d->now);
    p->slot = (int16_t)ctx.found;
    return 0;
}

int16_t mt_pb_dns_control(MtDrv *d, MtAnyPb *pb) {
    switch (pb->dnr.op) {
    case MT_DNR_A: case MT_DNR_PTR: case MT_DNR_HINFO: case MT_DNR_MX: return lookup(d, pb);
    case MT_DNR_ENUM:   return do_enum(d, pb);
    case MT_DNR_CANCEL: return do_cancel(d, pb);
    default:            return MT_PB_UNIMP;
    }
}

/* The departing application's lookups: forget them without a completion (its code is going away),
 * and tell the core to stop (it never writes `res` afterwards; nothing of ours points into the heap). */
void mt_pb_dns_goodbye(MtDrv *d) {
    uint32_t i;
    int any = 0;
    for (i = 0; i < MT_PB_DNS; i++) {
        MtDnsSlot *q = &d->dns[i];
        if (!q->state || !q->pb || !mt_os_app_mem(mt_os_addr(q->pb))) continue;
        cancel_core(d, q);
        q->state = 0;
        q->pb = NULL;
        any = 1;
    }
    if (any) kick(d);
}
