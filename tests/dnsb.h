#ifndef DNSB_H
#define DNSB_H
/* Shared DNS test helpers: message builder, query inspection, response delivery. */
#include "t.h"
#include "net.h"
#include "mt_int.h"
#include "mt_util.h"

#define T_DNS 0xC0A83235u          /* 192.168.50.53, on-link, MAC = T_PEER_MAC */

/* ---- tiny DNS message builder ---- */
typedef struct { uint8_t b[1400]; int n; } pkt;
static inline void p8(pkt *p, unsigned v) { p->b[p->n++] = (uint8_t)v; }
static inline void p16(pkt *p, unsigned v) { p8(p, v >> 8); p8(p, v); }
static inline void p32(pkt *p, uint32_t v) { p16(p, v >> 16); p16(p, v & 0xFFFF); }
static inline int pname(pkt *p, const char *s) {       /* returns the offset the name starts at */
    int start = p->n;
    while (*s) {
        const char *d = strchr(s, '.');
        size_t l = d ? (size_t)(d - s) : strlen(s);
        p8(p, (unsigned)l);
        memcpy(p->b + p->n, s, l);
        p->n += (int)l;
        s += l;
        if (*s == '.') s++;
    }
    p8(p, 0);
    return start;
}
static inline void pptr(pkt *p, int off) { p16(p, 0xC000u | (unsigned)off); }
/* header + question; flags e.g. 0x8180 (QR RD RA), rcode in the low bits */
static inline void begin(pkt *p, uint16_t id, uint16_t flags, const char *qname, unsigned qtype) {
    p->n = 0;
    p16(p, id); p16(p, flags); p16(p, 1); p16(p, 0); p16(p, 0); p16(p, 0);
    pname(p, qname);
    p16(p, qtype); p16(p, 1);
}
static inline void count_an(pkt *p, unsigned n) { mt_wr16(p->b + 6, (uint16_t)n); }
/* RR header with a 0xC00C owner pointer; returns the rdlength offset (patch with endrr) */
static inline int rr(pkt *p, unsigned type, uint32_t ttl) {
    int at;
    pptr(p, 12); p16(p, type); p16(p, 1); p32(p, ttl);
    at = p->n;
    p16(p, 0);
    return at;
}
static inline void endrr(pkt *p, int at) { mt_wr16(p->b + at, (uint16_t)(p->n - at - 2)); }
static inline void rr_a(pkt *p, uint32_t ttl, mt_ip4 a) { int at = rr(p, 1, ttl); p32(p, a); endrr(p, at); }

/* ---- query inspection ---- */
typedef struct { uint16_t id, sport, dport, qtype; char name[256]; mt_ip4 dst; int ok; } qv;
static inline void get_q(fake_link *fl, int i, qv *q) {
    ipv v;
    const uint8_t *d, *w;
    int o = 0;
    memset(q, 0, sizeof *q);
    if (i >= fl->n || !view_ip(fl->f[i], fl->len[i], &v) || v.proto != 17) return;
    q->dst = v.dst;
    q->sport = mt_rd16(v.payload);
    q->dport = mt_rd16(v.payload + 2);
    d = v.payload + 8;
    q->id = mt_rd16(d);
    if (mt_rd16(d + 2) != 0x0100 || mt_rd16(d + 4) != 1) return;
    w = d + 12;
    while (*w) {
        if (o) q->name[o++] = '.';
        memcpy(q->name + o, w + 1, *w);
        o += *w;
        w += 1 + *w;
    }
    q->name[o] = 0;
    q->qtype = mt_rd16(w + 1);
    q->ok = mt_rd16(w + 3) == 1 && (int)(w + 5 - d) + 8 == (int)v.len;
}

/* deliver a DNS response (from src:sport) to the port the query i came from; odd address */
static inline void deliver(mt_stack *s, const qv *q, mt_ip4 src, uint16_t sport, const pkt *p,
                    uint32_t now) {
    static uint8_t buf[1700];
    uint8_t pl[1500];
    uint8_t *f = buf + 1;
    uint16_t l;
    mt_wr16(pl, sport); mt_wr16(pl + 2, q->sport);
    mt_wr16(pl + 4, (uint16_t)(8 + p->n)); mt_wr16(pl + 6, 0);
    memcpy(pl + 8, p->b, (size_t)p->n);
    l = mk_ip(f, src, T_ME, 17, 7, 0, pl, (uint16_t)(8 + p->n));
    mt_input(s, f, l, now);
}
static inline void reply(mt_stack *s, const qv *q, const pkt *p, uint32_t now) {
    deliver(s, q, T_DNS, 53, p, now);
}

typedef struct { int n; mt_dns_result r; } got;
static inline void on_done(void *user, const mt_dns_result *r) { got *g = (got *)user; g->n++; g->r = *r; }

static inline mt_stack *setup(fake_link *fl, const char *domain) {
    mt_stack *s = tstack_new(fl, T_ME);
    mt_ifconfig c;
    mt_get_config(s, &c);
    c.dns = T_DNS;
    memset(c.domain, 0, sizeof c.domain);
    if (domain) strcpy(c.domain, domain);
    mt_set_config(s, &c);
    mt_arp_insert(s, T_DNS, T_PEER_MAC, 0);
    return s;
}

#endif
