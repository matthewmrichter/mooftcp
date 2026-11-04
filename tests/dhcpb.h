/* dhcpb.h - a scripted DHCP server for the host tests (test_dhcp.c, test_pb_dhcp.c): parse the
 * client's BOOTP requests from the fake link, build replies as Ethernet frames. Header-only (static). */
#ifndef DHCPB_H
#define DHCPB_H

#include <string.h>
#include "net.h"
#include "mt_util.h"

#define SRV      T_PEER               /* 192.168.50.10: the DHCP server, on-link */
#define OFFERED  0xC0A83264u          /* 192.168.50.100 */
#define OTHER    0xC0A83265u          /* 192.168.50.101 */
#define DNS1     0x01010101u
#define BCAST    0xFFFFFFFFu
#define MAGIC    0x63825363u

/* ---- the client's messages ---- */
typedef struct {
    int ok;                      /* a well-formed BOOTP request from port 68 to 67 */
    int cksum_ok;
    int eth_bcast;
    mt_ip4 ipsrc, ipdst;
    uint8_t op, type;
    uint32_t xid;
    uint16_t secs, flags, blen;
    mt_ip4 ciaddr, yiaddr, req, sid;
    int has_req, has_sid, has_cid, has_maxsz;
    uint8_t prl[32]; int nprl;
    uint8_t chaddr[6];
} dmsg;

static inline int parse_client(const uint8_t *f, uint16_t len, dmsg *m) {
    ipv ip;
    const uint8_t *u, *b, *o;
    uint16_t ulen, n, i;
    uint32_t sum;
    memset(m, 0, sizeof *m);
    if (!view_ip(f, len, &ip) || ip.proto != 17 || ip.len < 8) return 0;
    u = ip.payload;
    ulen = mt_rd16(u + 4);
    if (mt_rd16(u) != 68 || mt_rd16(u + 2) != 67 || ulen > ip.len || ulen < 8 + 240) return 0;
    sum = (ip.src >> 16) + (ip.src & 0xFFFFu) + (ip.dst >> 16) + (ip.dst & 0xFFFFu) + 17u + ulen;
    m->cksum_ok = mt_rd16(u + 6) != 0 && mt_cksum_fold(mt_cksum_add(sum, u, ulen)) == 0;
    m->eth_bcast = (f[0] & f[1] & f[2] & f[3] & f[4] & f[5]) == 0xFF;
    m->ipsrc = ip.src;
    m->ipdst = ip.dst;
    b = u + 8;
    n = (uint16_t)(ulen - 8);
    m->blen = n;
    m->op = b[0];
    m->xid = mt_rd32(b + 4);
    m->secs = mt_rd16(b + 8);
    m->flags = mt_rd16(b + 10);
    m->ciaddr = mt_rd32(b + 12);
    m->yiaddr = mt_rd32(b + 16);
    memcpy(m->chaddr, b + 28, 6);
    if (b[1] != 1 || b[2] != 6 || mt_rd32(b + 236) != MAGIC) return 0;
    o = b + 240;
    for (i = 0; i < n - 240;) {
        uint8_t c = o[i], l;
        if (c == 0) { i++; continue; }
        if (c == 255) break;
        if (i + 1u >= n - 240u) return 0;
        l = o[i + 1];
        if (i + 2u + l > n - 240u) return 0;
        switch (c) {
        case 53: if (l >= 1) m->type = o[i + 2]; break;
        case 50: if (l == 4) { m->req = mt_rd32(o + i + 2); m->has_req = 1; } break;
        case 54: if (l == 4) { m->sid = mt_rd32(o + i + 2); m->has_sid = 1; } break;
        case 61: if (l == 7 && o[i + 2] == 1 && memcmp(o + i + 3, T_ME_MAC, 6) == 0) m->has_cid = 1; break;
        case 57: if (l == 2 && mt_rd16(o + i + 2) >= 576) m->has_maxsz = 1; break;
        case 55: if (l <= sizeof m->prl) { memcpy(m->prl, o + i + 2, l); m->nprl = l; } break;
        default: break;
        }
        i = (uint16_t)(i + 2u + l);
    }
    m->ok = m->op == 1;
    return m->ok;
}

/* The k-th DHCP message among the frames sent (0-based), or 0 if there is none. */
static inline int nth_dhcp(const fake_link *fl, int k, dmsg *m) {
    int i;
    for (i = 0; i < fl->n; i++)
        if (parse_client(fl->f[i], fl->len[i], m) && k-- == 0) return 1;
    memset(m, 0, sizeof *m);
    return 0;
}
static inline int count_dhcp(const fake_link *fl) {
    dmsg m;
    int i, n = 0;
    for (i = 0; i < fl->n; i++) n += parse_client(fl->f[i], fl->len[i], &m);
    return n;
}
static inline int prl_has(const dmsg *m, uint8_t c) {
    int i;
    for (i = 0; i < m->nprl; i++) if (m->prl[i] == c) return 1;
    return 0;
}

/* ---- the server's replies ---- */
typedef struct {
    uint8_t op, type, chaddr[6];
    uint32_t xid, cookie;
    mt_ip4 yiaddr, sid, mask, router, dns;
    const char *domain;
    uint32_t lease, t1, t2;
    int has_sid, has_mask, has_router, has_dns, has_lease, has_t1, has_t2, no_type;
    int bad_cksum, no_cksum;
    const uint8_t *extra; uint16_t nextra;    /* raw option bytes before the end option */
} drep;

static inline void rep_init(drep *r, uint8_t type, uint32_t xid) {
    memset(r, 0, sizeof *r);
    r->op = 2;
    r->type = type;
    r->xid = xid;
    r->cookie = MAGIC;
    memcpy(r->chaddr, T_ME_MAC, 6);
    r->yiaddr = OFFERED;
    r->sid = SRV; r->has_sid = 1;
    r->mask = 0xFFFFFF00u; r->has_mask = 1;
    r->router = T_GW; r->has_router = 1;
    r->dns = DNS1; r->has_dns = 1;
    r->domain = "home.lan";
    r->lease = 3600; r->has_lease = 1;
}

static inline uint16_t opt4(uint8_t *o, uint8_t c, uint32_t v) {
    o[0] = c; o[1] = 4; mt_wr32(o + 2, v);
    return 6;
}

/* Build the reply as an Ethernet frame (eth dst = our MAC, or broadcast for a broadcast IP dst). */
static inline uint16_t mk_reply(uint8_t *f, mt_ip4 ipsrc, mt_ip4 ipdst, const drep *r) {
    static uint8_t u[1400];
    uint8_t *b = u + 8, *o;
    uint16_t n = 240, ulen, len;
    uint32_t sum;
    memset(u, 0, sizeof u);
    b[0] = r->op; b[1] = 1; b[2] = 6;
    mt_wr32(b + 4, r->xid);
    mt_wr32(b + 16, r->yiaddr);
    memcpy(b + 28, r->chaddr, 6);
    mt_wr32(b + 236, r->cookie);
    o = b;
    if (!r->no_type) { o[n] = 53; o[n + 1] = 1; o[n + 2] = r->type; n = (uint16_t)(n + 3); }
    if (r->has_sid) n = (uint16_t)(n + opt4(o + n, 54, r->sid));
    if (r->has_mask) n = (uint16_t)(n + opt4(o + n, 1, r->mask));
    if (r->has_router) n = (uint16_t)(n + opt4(o + n, 3, r->router));
    if (r->has_dns) n = (uint16_t)(n + opt4(o + n, 6, r->dns));
    if (r->has_lease) n = (uint16_t)(n + opt4(o + n, 51, r->lease));
    if (r->has_t1) n = (uint16_t)(n + opt4(o + n, 58, r->t1));
    if (r->has_t2) n = (uint16_t)(n + opt4(o + n, 59, r->t2));
    if (r->domain) {
        uint8_t k = (uint8_t)strlen(r->domain);
        o[n] = 15; o[n + 1] = k; memcpy(o + n + 2, r->domain, k);
        n = (uint16_t)(n + 2u + k);
    }
    if (r->nextra) { memcpy(o + n, r->extra, r->nextra); n = (uint16_t)(n + r->nextra); }
    o[n++] = 255;
    ulen = (uint16_t)(8 + n);
    mt_wr16(u, 67);
    mt_wr16(u + 2, 68);
    mt_wr16(u + 4, ulen);
    mt_wr16(u + 6, 0);
    if (!r->no_cksum) {
        uint16_t c;
        sum = (ipsrc >> 16) + (ipsrc & 0xFFFFu) + (ipdst >> 16) + (ipdst & 0xFFFFu) + 17u + ulen;
        c = mt_cksum_fold(mt_cksum_add(sum, u, ulen));
        if (c == 0) c = 0xFFFF;
        if (r->bad_cksum) c ^= 0x0101;
        mt_wr16(u + 6, c);
    }
    len = mk_ip(f, ipsrc, ipdst, 17, 1, 0, u, ulen);
    if (ipdst == BCAST) memset(f, 0xFF, 6);
    return len;
}

#endif
