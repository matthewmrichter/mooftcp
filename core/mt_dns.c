#include <string.h>
#include "mt_int.h"

/* DNS stub resolver (RFC 1035) over the stack's single lazily created UDP endpoint.
 *
 * Memory (all in the arena, see mt_layout_calc): mt_dns = udp ptr + RNG + 4 query slots
 * (each keeps the query name once, 256 bytes) + the 1024-byte receive buffer; the cache is a
 * separate array of lim.dns_cache entries (key and canonical name, each <= 63 chars).
 *
 * Wire access is bytewise (mt_rd/mt_wr) on uint8_t*: packets may sit at odd addresses.
 * Every read of a response is bounds-checked against the datagram length; compression pointers
 * must point strictly backwards (so no loop is possible) and at most 16 are followed; a
 * decoded name is limited to 255 wire bytes. A response that fails any check is IGNORED (the
 * query keeps retrying/timing out) - with one exception: a truncated (TC) response is used for
 * whatever complete records precede the damage (there is no TCP fallback).
 *
 * Retry schedule per question: sent at t0, again at t0+2 s and t0+6 s, TIMEOUT at t0+14 s.
 * Dotless names are first asked as name.domain (when a domain is configured) and, on any
 * negative answer (NXDOMAIN, no data, SERVFAIL), as the bare name with a fresh ID and schedule;
 * a TIMEOUT of the first question is final. A name with a dot or a trailing dot is absolute. */

#define DNS_PORT 53u
#define DNS_HDR 12u
#define DNS_TXOFF 768u            /* query is built in txbuf here, clear of the IP payload area */
#define DNS_MAXTEXT 253u          /* longest text name (255 wire bytes) */
#define DNS_TTL_MIN 60u
#define DNS_TTL_MAX 86400u
#define DNS_CNAME_HOPS 8u
#define DNS_MAXJUMPS 16u

static const uint32_t dns_sched[3] = {2000u, 6000u, 14000u};

static char lc(char c) { return (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c); }

/* ---- name syntax ---- */

/* Valid host name text: 1..253 chars, labels 1..63, no spaces/control characters. */
static int name_ok(const char *s, size_t n) {
    size_t i, lab = 0;
    if (n == 0 || n > DNS_MAXTEXT) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20u || c == 0x7Fu) return 0;
        if (c == '.') {
            if (lab == 0) return 0;
            lab = 0;
        } else if (++lab > 63u) {
            return 0;
        }
    }
    return lab != 0;
}

/* Dotted quad: exactly four non-empty all-digit parts. 1 = quad (*ip set); -1 = looks like a
 * quad but a part has more than 3 digits or exceeds 255; 0 = anything else (a host name). */
static int parse_quad(const char *s, size_t n, mt_ip4 *ip) {
    uint32_t acc = 0, part = 0, digits = 0, dots = 0;
    int bad = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == '.') {
            if (digits == 0) return 0;
            acc = (acc << 8) | (part & 0xFFu);
            dots++;
            part = digits = 0;
        } else if (c >= '0' && c <= '9') {
            if (++digits > 3u) bad = 1; else part = part * 10u + (uint32_t)(c - '0');
            if (part > 255u) bad = 1;
        } else {
            return 0;
        }
    }
    if (dots != 3u || digits == 0) return 0;
    if (bad) return -1;
    *ip = (acc << 8) | part;
    return 1;
}

/* Configured search domain, or 0 when none/invalid. Valid means a plain name (no trailing dot). */
static size_t dom_len(const mt_stack *s) {
    size_t n = 0;
    while (n < sizeof s->cfg.domain && s->cfg.domain[n]) n++;
    if (n == sizeof s->cfg.domain || !name_ok(s->cfg.domain, n)) return 0;
    return n;
}

/* Length of the domain this question appends (0 = bare name). */
static size_t q_dom(const mt_stack *s, const mt_dns_q *q) {
    size_t dl;
    if (!q->dom) return 0;
    dl = dom_len(s);
    return dl && strlen(q->name) + 1u + dl <= DNS_MAXTEXT ? dl : 0;
}

/* The name this question asks for ("name" or "name.domain") into dst (>= 256 bytes). */
static void q_attempt(const mt_stack *s, const mt_dns_q *q, char *dst) {
    size_t n = strlen(q->name), dl = q_dom(s, q);
    memcpy(dst, q->name, n);
    if (dl) { dst[n] = '.'; memcpy(dst + n + 1, s->cfg.domain, dl); n += 1u + dl; }
    dst[n] = 0;
}

/* ---- wire names ---- */

/* Decode (dst != NULL: text into dst, capacity cap) or compare (dst == NULL: case-insensitively
 * with cmp) the name at pkt+off. *end = offset just past the name in the original position.
 * Returns -1 malformed, 0 compare mismatch, 1 ok. */
static int wire_name(const uint8_t *pkt, uint16_t len, uint32_t off, char *dst, uint32_t cap,
                     const char *cmp, uint16_t *end) {
    uint32_t pos = off, out = 0, wire = 1, jumps = 0, l, k;
    int jumped = 0, match = 1;
    for (;;) {
        unsigned b;
        if (pos >= len) return -1;
        b = pkt[pos];
        if (b == 0) {
            if (!jumped) *end = (uint16_t)(pos + 1u);
            break;
        }
        if ((b & 0xC0u) == 0xC0u) {
            uint32_t target;
            if (pos + 1u >= len) return -1;
            target = ((uint32_t)(b & 0x3Fu) << 8) | pkt[pos + 1u];
            if (!jumped) { *end = (uint16_t)(pos + 2u); jumped = 1; }
            if (target >= pos || ++jumps > DNS_MAXJUMPS) return -1;   /* backwards only */
            pos = target;
            continue;
        }
        if (b & 0xC0u) return -1;                      /* reserved label types */
        l = b;
        if (pos + 1u + l > len) return -1;
        wire += l + 1u;
        if (wire > 255u) return -1;
        if (dst) {
            if (out + (out ? 1u : 0u) + l + 1u > cap) return -1;
            if (out) dst[out++] = '.';
            for (k = 0; k < l; k++) {
                if (pkt[pos + 1u + k] == 0) return -1;
                dst[out++] = (char)pkt[pos + 1u + k];
            }
        } else if (match) {
            if (out) { if (cmp[out] != '.') match = 0; else out++; }
            for (k = 0; match && k < l; k++) {
                if (cmp[out] == 0 || lc(cmp[out]) != lc((char)pkt[pos + 1u + k])) match = 0;
                else out++;
            }
        }
        pos += 1u + l;
    }
    if (dst) { dst[out] = 0; return 1; }
    return match && cmp[out] == 0;
}

/* Encode text labels at w; returns the new end. Each text segment must be valid (name_ok). */
static uint8_t *enc_labels(uint8_t *w, const char *s, size_t n) {
    uint8_t *lp = w++;
    size_t i;
    uint8_t cnt = 0;
    for (i = 0; i < n; i++) {
        if (s[i] == '.') { *lp = cnt; lp = w++; cnt = 0; }
        else { *w++ = (uint8_t)s[i]; cnt++; }
    }
    *lp = cnt;
    return w;
}

/* ---- sending ---- */

static uint16_t dns_next_id(mt_dns *d) {
    uint32_t x = d->rnd;
    uint16_t id;
    unsigned tries = 0, i;
    do {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;       /* xorshift32 */
        id = (uint16_t)(x >> 8);
        for (i = 0; i < MT_DNS_SLOTS; i++)
            if (d->q[i].used && d->q[i].id == id) { id = 0; break; }
    } while (id == 0 && ++tries < 16u);
    d->rnd = x;
    return id;
}

/* Build the question in txbuf (away from the IP payload area) and send it. */
static int dns_send(mt_stack *s, mt_dns_q *q, uint32_t now) {
    uint8_t *w = s->txbuf + DNS_TXOFF, *e;
    size_t dl = q_dom(s, q);
    mt_wr16(w, q->id); mt_wr16(w + 2, 0x0100u);        /* RD */
    mt_wr16(w + 4, 1); mt_wr16(w + 6, 0); mt_wr16(w + 8, 0); mt_wr16(w + 10, 0);
    e = enc_labels(w + DNS_HDR, q->name, strlen(q->name));
    if (dl) e = enc_labels(e, s->cfg.domain, dl);
    *e++ = 0;
    mt_wr16(e, q->qtype); mt_wr16(e + 2, 1);
    e += 4;
    return mt_udp_send(s->dns->udp, s->cfg.dns, DNS_PORT, w, (uint16_t)(e - w), 1, now);
}

/* ---- cache ---- */

/* Cache key = the FQDN that was (or would be) asked: name, or name.domain when dl != 0;
 * lower-cased, no trailing dot, 63 chars max (else 0 = not cacheable). */
static int key_make(const char *name, size_t n, const char *dom, size_t dl, char *key) {
    size_t i;
    if (n + (dl ? 1u + dl : 0u) >= MT_DNS_CNAME) return 0;
    for (i = 0; i < n; i++) key[i] = lc(name[i]);
    if (dl) {
        key[n] = '.';
        for (i = 0; i < dl; i++) key[n + 1u + i] = lc(dom[i]);
        n += 1u + dl;
    }
    key[n] = 0;
    return 1;
}

/* Live = in use and not yet expired. The bound on exp - now keeps an entry that expired
 * long ago from looking live again when the 32-bit ms clock wraps (after 2^31 ms); an entry
 * found expired is freed on the spot. */
static int cent_live(mt_dns_cent *c, uint32_t now) {
    if (c->naddr == 0) return 0;
    if (mt_time_before(now, c->exp) && c->exp - now <= DNS_TTL_MAX * 1000u) return 1;
    c->naddr = 0;
    return 0;
}

static int cache_lookup(mt_stack *s, const char *name, size_t n, size_t dl, mt_dns_result *out,
                        uint32_t now) {
    char key[MT_DNS_CNAME];
    uint16_t i;
    if (!key_make(name, n, s->cfg.domain, dl, key)) return 0;
    for (i = 0; i < s->lim.dns_cache; i++) {
        mt_dns_cent *c = &s->dcache[i];
        if (cent_live(c, now) && !c->ptr && strcmp(c->name, key) == 0) {
            memset(out, 0, sizeof *out);
            out->status = MT_DNS_OK;
            strcpy(out->cname, c->canon);
            memcpy(out->addr, c->addr, sizeof out->addr);
            out->naddr = c->naddr;
            c->used = now;
            return 1;
        }
    }
    return 0;
}

/* The entry to (over)write: the one holding the same key (name, or address for PTR), else a
 * free/expired one, else the least recently used. */
static mt_dns_cent *cent_slot(mt_stack *s, const char *key, mt_ip4 addr, int ptr, uint32_t now) {
    mt_dns_cent *v = NULL;
    uint16_t i;
    for (i = 0; i < s->lim.dns_cache; i++) {
        const mt_dns_cent *c = &s->dcache[i];
        if (c->naddr && (ptr ? (c->ptr && c->addr[0] == addr) : (!c->ptr && strcmp(c->name, key) == 0)))
            return &s->dcache[i];
    }
    for (i = 0; !v && i < s->lim.dns_cache; i++)
        if (!cent_live(&s->dcache[i], now)) v = &s->dcache[i];
    if (!v) {                                          /* LRU */
        v = &s->dcache[0];
        for (i = 1; i < s->lim.dns_cache; i++)
            if (mt_time_before(s->dcache[i].used, v->used)) v = &s->dcache[i];
    }
    return v;
}

/* Remember an A result under the FQDN that answered (q->name plus the search domain if one
 * was used). Not cached when that name or the canonical name is longer than 63 characters.
 * TTL is clamped to 60 s..24 h. */
static void cache_store(mt_stack *s, const mt_dns_q *q, const mt_dns_result *r, uint32_t ttl,
                        uint32_t now) {
    char key[MT_DNS_CNAME];
    mt_dns_cent *v;
    if (s->lim.dns_cache == 0 || r->naddr == 0 || strlen(r->cname) >= MT_DNS_CNAME ||
        !key_make(q->name, strlen(q->name), s->cfg.domain, q_dom(s, q), key))
        return;
    if (ttl < DNS_TTL_MIN) ttl = DNS_TTL_MIN;
    if (ttl > DNS_TTL_MAX) ttl = DNS_TTL_MAX;
    v = cent_slot(s, key, 0, 0, now);
    strcpy(v->name, key);
    strcpy(v->canon, r->cname);
    memcpy(v->addr, r->addr, sizeof v->addr);
    v->naddr = r->naddr;
    v->ptr = 0;
    v->exp = now + ttl * 1000u;
    v->used = now;
}

/* Remember a PTR answer (name <= 63 characters) keyed on the address; addr[0] holds the key. */
static void cache_store_ptr(mt_stack *s, mt_ip4 addr, const char *name, uint32_t ttl,
                            uint32_t now) {
    mt_dns_cent *v;
    if (s->lim.dns_cache == 0 || name[0] == 0 || strlen(name) >= MT_DNS_CNAME) return;
    if (ttl < DNS_TTL_MIN) ttl = DNS_TTL_MIN;
    if (ttl > DNS_TTL_MAX) ttl = DNS_TTL_MAX;
    v = cent_slot(s, NULL, addr, 1, now);
    v->name[0] = 0;
    strcpy(v->canon, name);
    memset(v->addr, 0, sizeof v->addr);
    v->addr[0] = addr;
    v->naddr = 1;
    v->ptr = 1;
    v->exp = now + ttl * 1000u;
    v->used = now;
}

/* Copy src into the caller's name buffer: at most cap-1 characters and a NUL (cap >= 1). */
static void name_out(char *dst, uint16_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) n = (size_t)cap - 1u;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static int cache_lookup_ptr(mt_stack *s, mt_ip4 addr, char *dst, uint16_t cap, uint32_t now) {
    uint16_t i;
    for (i = 0; i < s->lim.dns_cache; i++) {
        mt_dns_cent *c = &s->dcache[i];
        if (cent_live(c, now) && c->ptr && c->addr[0] == addr) {
            name_out(dst, cap, c->canon);
            c->used = now;
            return 1;
        }
    }
    return 0;
}

void mt_dns_enum_cache(mt_stack *s, mt_dns_enum_cb cb, void *user, uint32_t now) {
    uint16_t i;
    for (i = 0; i < s->lim.dns_cache; i++) {
        mt_dns_cent *c = &s->dcache[i];
        if (cent_live(c, now) && !c->ptr)
            cb(user, c->name, c->addr, c->naddr, (c->exp - now + 999u) / 1000u);
    }
}

/* ---- response parsing ---- */

/* Validate and digest response p (len bytes) against query q. Returns 0 to ignore it, else 1
 * with *st the outcome and q->out filled (A/PTR/HINFO/MX fields). *ttl = smallest A TTL. */
static int dns_parse_raw(mt_stack *s, mt_dns_q *q, const uint8_t *p, uint16_t len,
                     mt_dns_status *st, uint32_t *ttl) {
    mt_dns_result *o = q->out;                                   /* NULL for a reverse lookup */
    char *cn = o ? o->cname : s->dns->pname;
    uint32_t cncap = o ? (uint32_t)sizeof o->cname : (uint32_t)sizeof s->dns->pname;
    uint32_t off, an, i, found = 0, hops = 0, mxbest = 0, tmin = 0xFFFFFFFFu;
    unsigned flags, rcode, tc;
    uint16_t end;
    if (len < DNS_HDR + 5u) return 0;
    flags = mt_rd16(p + 2);
    if (!(flags & 0x8000u) || (flags & 0x7800u)) return 0;       /* QR=1, opcode 0 */
    if (mt_rd16(p + 4) != 1) return 0;
    an = mt_rd16(p + 6);
    rcode = flags & 0xFu;
    tc = flags & 0x0200u;
    if (o) memset(o, 0, sizeof *o);
    q_attempt(s, q, cn);                                   /* current target name */
    if ((p[DNS_HDR] & 0xC0u) != 0) return 0;                     /* question: plain labels */
    if (wire_name(p, len, DNS_HDR, NULL, 0, cn, &end) != 1) return 0;
    off = end;
    if (off + 4u > len || mt_rd16(p + off) != q->qtype || mt_rd16(p + off + 2) != 1) return 0;
    off += 4u;
    *ttl = 0;
    if (rcode == 3u) { *st = MT_DNS_NOT_FOUND; return 1; }
    if (rcode != 0) { *st = MT_DNS_NO_ANSWER; return 1; }
    for (i = 0; i < an; i++) {
        uint32_t type, rdl, rd, rttl;
        int own, r;
        if (off >= len) goto bad;
        own = wire_name(p, len, off, NULL, 0, cn, &end);   /* owner == current target? */
        if (own < 0) goto bad;
        off = end;
        if (off + 10u > len) goto bad;
        type = mt_rd16(p + off);
        rttl = mt_rd32(p + off + 4);
        rdl = mt_rd16(p + off + 8);
        off += 10u;
        if (off + rdl > len) goto bad;
        rd = off;
        off += rdl;
        if (!own || mt_rd16(p + rd - 8) != 1) continue;          /* other owner / not class IN */
        if (type == 5u) {                                        /* CNAME: follow */
            if (++hops > DNS_CNAME_HOPS) goto bad;
            r = wire_name(p, len, rd, cn, cncap, NULL, &end);
            if (r != 1 || end > off) goto bad;
        } else if (type != q->qtype) {
            continue;
        } else if (type == 1u) {                                 /* A */
            if (rdl != 4u) goto bad;
            if (o->naddr < MT_DNS_MAXADDR) o->addr[o->naddr++] = mt_rd32(p + rd);
            if (rttl < tmin) tmin = rttl;
            found = 1;
        } else if (type == 12u) {                                /* PTR -> cname */
            if (found) continue;
            r = wire_name(p, len, rd, cn, cncap, NULL, &end);
            if (r != 1 || end > off) goto bad;
            tmin = rttl;
            found = 1;
        } else if (type == 13u) {                                /* HINFO: cpu, os */
            uint32_t l1, l2, k;
            if (found) continue;
            if (rdl < 2u) goto bad;
            l1 = p[rd];
            if (1u + l1 + 1u > rdl) goto bad;
            l2 = p[rd + 1u + l1];
            if (2u + l1 + l2 > rdl) goto bad;
            for (k = 0; k < l1 && k < sizeof o->cpu - 1u; k++) o->cpu[k] = (char)p[rd + 1u + k];
            for (k = 0; k < l2 && k < sizeof o->os - 1u; k++) o->os[k] = (char)p[rd + 2u + l1 + k];
            found = 1;
        } else if (type == 15u) {                                /* MX: lowest preference */
            uint32_t pref;
            if (rdl < 3u) goto bad;
            pref = mt_rd16(p + rd);
            if (found && pref >= mxbest) continue;
            r = wire_name(p, len, rd + 2u, o->mx_name, sizeof o->mx_name, NULL, &end);
            if (r != 1 || end > off) goto bad;
            mxbest = pref;
            o->mx_pref = (uint16_t)pref;
            found = 1;
        }
    }
    goto done;
bad:
    if (!tc) return 0;                       /* damaged and not truncated: not trusted */
done:
    *st = found ? MT_DNS_OK : MT_DNS_NO_ANSWER;
    *ttl = tmin == 0xFFFFFFFFu ? 0u : tmin;
    return 1;
}

/* The state of a result while its query is outstanding: all zero, status PENDING. */
static void dns_pending(mt_dns_result *o) {
    memset(o, 0, sizeof *o);
    o->status = MT_DNS_PENDING;
}

/* dns_parse_raw works directly in the caller's result (to save stack). A rejected response
 * must not be visible there: restore the pending state (all zero, status PENDING) that
 * mt_dns_query left, so polling the result mid-query never shows spoofed or partial data. */
static int dns_parse(mt_stack *s, mt_dns_q *q, const uint8_t *p, uint16_t len,
                     mt_dns_status *st, uint32_t *ttl) {
    if (dns_parse_raw(s, q, p, len, st, ttl)) return 1;
    if (q->out) dns_pending(q->out);
    return 0;
}

/* ---- completion ---- */

typedef struct { mt_dns_cb cb; mt_dns_ptr_cb pcb; void *user; mt_dns_result *out; char *pbuf;
                 mt_dns_status st; } dns_done;

/* Free the slot and fix up the result; the caller invokes the callback afterwards (so the
 * callback may start new queries). */
static void dns_take(mt_stack *s, mt_dns_q *q, mt_dns_status st, dns_done *d) {
    if (st != MT_DNS_OK) mt_log(s, MT_LOG_DNS_FAIL, (uint8_t)st, q->id);
    d->cb = q->cb; d->pcb = q->pcb; d->user = q->user; d->out = q->out; d->pbuf = q->pbuf; d->st = st;
    if (q->pbuf) name_out(q->pbuf, q->pcap, st == MT_DNS_OK ? s->dns->pname : "");
    if (q->out) {
        if (st != MT_DNS_OK) memset(q->out, 0, sizeof *q->out);
        q->out->status = st;
    }
    q->used = 0;
}
static void dns_fire(const dns_done *d) {
    if (d->pcb) d->pcb(d->user, d->st, d->pbuf);
    else if (d->cb) d->cb(d->user, d->out);
}

/* Handle one received datagram. A completed query is described in *d (d->cb may be NULL even
 * then, so `*fired` says whether it completed). */
static int dns_rx(mt_stack *s, const mt_udp_dgram *g, dns_done *d) {
    mt_dns *dn = s->dns;
    mt_dns_q *q = NULL;
    mt_dns_status st;
    uint32_t ttl, i;
    if (g->len < DNS_HDR || s->cfg.dns == 0 || g->src != s->cfg.dns || g->sport != DNS_PORT)
        return 0;
    for (i = 0; i < MT_DNS_SLOTS; i++)
        if (dn->q[i].used && dn->q[i].id == mt_rd16(g->data)) { q = &dn->q[i]; break; }
    if (!q || !dns_parse(s, q, g->data, g->len, &st, &ttl)) return 0;
    if (st == MT_DNS_OK && q->qtype == MT_DNS_Q_A) cache_store(s, q, q->out, ttl, s->now);
    if (st == MT_DNS_OK && q->pbuf) cache_store_ptr(s, q->paddr, s->dns->pname, ttl, s->now);
    if (st != MT_DNS_OK && q->dom && q_dom(s, q)) {            /* negative: try the bare name */
        dns_pending(q->out);                /* parse left the negative answer in `out` */
        q->dom = 0;
        q->id = dns_next_id(dn);
        q->tries = 0;
        q->t0 = s->now;
        if (dns_send(s, q, s->now) == MT_OK) return 0;
    }
    dns_take(s, q, st, d);
    return 1;
}

static void dns_udp_cb(mt_udp *u, mt_udp_event ev, uint32_t arg, void *user) {
    mt_stack *s = (mt_stack *)user;
    mt_udp_dgram g;
    (void)arg;
    if (ev != MT_UDP_EV_DATA) return;
    while (mt_udp_peek(u, &g)) {
        dns_done d;
        int done = dns_rx(s, &g, &d);
        mt_udp_done(u);                                 /* before the user callback runs */
        if (done) dns_fire(&d);
    }
}

/* ---- timers ---- */

void mt_dns_timers(mt_stack *s, uint32_t now) {
    uint32_t i;
    for (i = 0; i < s->lim.dns_cache; i++) (void)cent_live(&s->dcache[i], now);   /* free expired */
    for (i = 0; i < MT_DNS_SLOTS; i++) {
        mt_dns_q *q = &s->dns->q[i];
        uint32_t el;
        if (!q->used || mt_time_before(now, q->t0)) continue;
        el = now - q->t0;
        if (el >= dns_sched[2]) {
            dns_done d;
            dns_take(s, q, MT_DNS_TIMEOUT, &d);
            dns_fire(&d);
        } else if (el >= dns_sched[1] && q->tries < 2u) {
            q->tries = 2u;
            (void)dns_send(s, q, now);
        } else if (el >= dns_sched[0] && q->tries < 1u) {
            q->tries = 1u;
            (void)dns_send(s, q, now);
        }
    }
}

uint32_t mt_dns_deadline(const mt_stack *s, uint32_t now) {
    uint32_t d = MT_NO_DEADLINE, i;
    for (i = 0; i < MT_DNS_SLOTS; i++) {
        const mt_dns_q *q = &s->dns->q[i];
        uint32_t due;
        if (!q->used) continue;
        due = q->t0 + dns_sched[q->tries];
        d = mt_dl_min(d, mt_time_before(due, now) ? 0u : due - now);
    }
    return d;
}

/* ---- API ---- */

int mt_dns_ptr_name(mt_ip4 a, char out[32]) {
    static const char suffix[] = "in-addr.arpa";
    int n = 0, i;
    for (i = 0; i < 4; i++) {
        unsigned v = (unsigned)((a >> (8 * i)) & 0xFFu);
        if (v >= 100u) out[n++] = (char)('0' + v / 100u);
        if (v >= 10u) out[n++] = (char)('0' + (v / 10u) % 10u);
        out[n++] = (char)('0' + v % 10u);
        out[n++] = '.';
    }
    memcpy(out + n, suffix, sizeof suffix);
    return n + (int)sizeof suffix - 1;
}

/* Start a network query: a free slot, the resolver's endpoint (created on first use in the
 * reserved UDP slot, on a random ephemeral port), the first send. *qp (if given) gets the slot. */
static int dns_launch(mt_stack *s, const char *name, size_t n, mt_dns_qtype qt, mt_dns_result *out,
                      mt_dns_cb cb, void *user, size_t dl, uint32_t now, mt_dns_q **qp) {   /* out NULL: reverse */
    mt_dns *dn = s->dns;
    mt_dns_q *q = NULL;
    uint32_t i;
    for (i = 0; i < MT_DNS_SLOTS; i++)
        if (!dn->q[i].used) { q = &dn->q[i]; break; }
    if (!q) return -MT_DNS_BUSY;
    if (!dn->udp) {
        uint16_t port = (uint16_t)(49152u + (dns_next_id(dn) % 16384u));
        dn->udp = mt_udp_create_reserved(s, port, dn->rx, MT_DNS_RXBUF, dns_udp_cb, s);
        if (!dn->udp) dn->udp = mt_udp_create_reserved(s, 0, dn->rx, MT_DNS_RXBUF, dns_udp_cb, s);
        if (!dn->udp) return -MT_DNS_BUSY;
    }
    memset(q, 0, sizeof *q);
    memcpy(q->name, name, n);
    if (out) dns_pending(out);
    q->out = out;
    q->cb = cb;
    q->user = user;
    q->qtype = (uint8_t)qt;
    q->t0 = now;
    q->id = dns_next_id(dn);
    q->dom = (uint8_t)(dl != 0);
    q->used = 1;
    if (dns_send(s, q, now) != MT_OK) { q->used = 0; return -MT_DNS_NO_SERVER; }
    if (qp) *qp = q;
    return MT_DNS_IN_PROGRESS;
}

int mt_dns_query(mt_stack *s, const char *name, mt_dns_qtype qt, mt_dns_result *out,
                 mt_dns_cb cb, void *user, uint32_t now) {
    size_t n = 0, dl;
    mt_ip4 ip;
    int absolute = 0, r;
    uint32_t i;

    s->now = now;
    if (!name || !out) return -MT_DNS_BAD_NAME;
    while (n <= MT_DNS_NAMELEN && name[n]) n++;            /* bounded strlen */
    if (n > MT_DNS_NAMELEN) return -MT_DNS_BAD_NAME;
    if (n && name[n - 1] == '.') { n--; absolute = 1; }    /* trailing dot: absolute name */
    if (qt != MT_DNS_Q_A && qt != MT_DNS_Q_PTR && qt != MT_DNS_Q_HINFO && qt != MT_DNS_Q_MX)
        return -MT_DNS_BAD_NAME;
    if (qt == MT_DNS_Q_A && (r = parse_quad(name, n, &ip)) != 0) {
        if (r < 0) return -MT_DNS_BAD_NAME;
        memset(out, 0, sizeof *out);
        out->status = MT_DNS_OK;
        memcpy(out->cname, name, n);
        out->addr[0] = ip;
        out->naddr = 1;
        return 0;
    }
    if (!name_ok(name, n)) return -MT_DNS_BAD_NAME;
    for (i = 0; i < n && !absolute; i++)                   /* a dot (or trailing dot): absolute */
        if (name[i] == '.') absolute = 1;
    dl = absolute ? 0u : dom_len(s);
    if (dl && n + 1u + dl > DNS_MAXTEXT) dl = 0;
    /* Cache: exactly the FQDN the network query would ask first (name.domain for a dotless
     * name with a domain set, else the bare/absolute name). */
    if (qt == MT_DNS_Q_A && cache_lookup(s, name, n, dl, out, now)) return 0;
    if (s->cfg.dns == 0) return -MT_DNS_NO_SERVER;
    return dns_launch(s, name, n, qt, out, cb, user, dl, now, NULL);
}

int mt_dns_ptr(mt_stack *s, mt_ip4 addr, char *namebuf, uint16_t cap, mt_dns_ptr_cb cb, void *user,
               uint32_t now) {
    mt_dns *dn = s->dns;
    mt_dns_q *q;
    char nm[32];
    uint32_t i;
    int r;
    s->now = now;
    if (!namebuf || cap == 0) return -MT_DNS_BAD_NAME;
    if (cache_lookup_ptr(s, addr, namebuf, cap, now)) return 0;
    if (s->cfg.dns == 0) return -MT_DNS_NO_SERVER;
    for (i = 0; i < MT_DNS_SLOTS; i++)                 /* the single scratch result is in use */
        if (dn->q[i].used && dn->q[i].pbuf) return -MT_DNS_BUSY;
    r = mt_dns_ptr_name(addr, nm);
    r = dns_launch(s, nm, (size_t)r, MT_DNS_Q_PTR, NULL, NULL, user, 0, now, &q);
    if (r == MT_DNS_IN_PROGRESS) { q->pbuf = namebuf; q->pcap = cap; q->paddr = addr; q->pcb = cb; }
    return r;
}

void mt_dns_cancel(mt_stack *s, const void *target) {
    uint32_t i;
    if (!target) return;
    for (i = 0; i < MT_DNS_SLOTS; i++) {
        mt_dns_q *q = &s->dns->q[i];
        if (q->used && ((const void *)q->out == target || (const void *)q->pbuf == target))
            q->used = 0;
    }
}

void mt_dns_reseed(mt_stack *s, uint32_t a, uint32_t b) {
    s->dns->rnd = ((a ^ 0x5DEECE66u) ^ (b * 0x9E3779B1u)) | 1u;
}
