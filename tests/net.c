#include <string.h>
#include "net.h"
#include "mt_util.h"
#include "mt_int.h"

const uint8_t T_ME_MAC[6]   = {0x02, 0, 0, 0, 0, 0x01};
const uint8_t T_PEER_MAC[6] = {0x02, 0, 0, 0, 0, 0x0A};
const uint8_t T_GW_MAC[6]   = {0x02, 0, 0, 0, 0, 0xFE};

static int fake_tx(void *ctx, const uint8_t *frame, uint16_t len) {
    fake_link *fl = (fake_link *)ctx;
    if (fl->n >= 64 || len > 1514) return 1;
    memcpy(fl->f[fl->n], frame, len);
    fl->len[fl->n] = len;
    fl->n++;
    return 0;
}

void fake_init(fake_link *fl, mt_link *link) {
    fake_clear(fl);
    link->ctx = fl;
    link->tx = fake_tx;
}

void fake_clear(fake_link *fl) {
    fl->n = 0;
    memset(fl->len, 0, sizeof fl->len);
}

mt_stack *tstack_new(fake_link *fl, mt_ip4 addr) {
    static uint64_t pool[4][20480 / 8];   /* 8-aligned: MT_ARENA_ALIGN */
    static int next;
    mt_limits lim;
    mt_link link;
    mt_ifconfig c;
    mt_stack *s;
    uint64_t *mem = pool[next++ & 3];
    mt_limits_default(&lim);
    fake_init(fl, &link);
    s = mt_stack_init(mem, sizeof pool[0], &lim, &link, 1);
    if (!s) return NULL;
    memset(&c, 0, sizeof c);
    c.addr = addr;
    c.mask = 0xFFFFFF00u;
    c.gateway = T_GW;
    c.mtu = 1500;
    memcpy(c.mac, T_ME_MAC, 6);
    mt_set_config(s, &c);
    return s;
}

uint16_t mk_arp(uint8_t *f, uint16_t op, const uint8_t smac[6], mt_ip4 sip,
                const uint8_t tmac[6], mt_ip4 tip) {
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    memcpy(f, op == 1 ? bcast : tmac, 6);
    memcpy(f + 6, smac, 6);
    mt_wr16(f + 12, 0x0806);
    mt_wr16(f + 14, 1);
    mt_wr16(f + 16, 0x0800);
    f[18] = 6;
    f[19] = 4;
    mt_wr16(f + 20, op);
    memcpy(f + 22, smac, 6);
    mt_wr32(f + 28, sip);
    memcpy(f + 32, tmac, 6);
    mt_wr32(f + 38, tip);
    return 42;
}

uint16_t mk_ip(uint8_t *f, mt_ip4 src, mt_ip4 dst, uint8_t proto, uint16_t id,
               uint16_t frag_off_flags, const uint8_t *pl, uint16_t plen) {
    uint8_t *ip = f + 14;
    memcpy(f, T_ME_MAC, 6);
    memcpy(f + 6, (src == T_GW || (src & 0xFFFFFF00u) != (T_ME & 0xFFFFFF00u))
                      ? T_GW_MAC : T_PEER_MAC, 6);
    mt_wr16(f + 12, 0x0800);
    ip[0] = 0x45;
    ip[1] = 0;
    mt_wr16(ip + 2, (uint16_t)(20 + plen));
    mt_wr16(ip + 4, id);
    mt_wr16(ip + 6, frag_off_flags);
    ip[8] = 64;
    ip[9] = proto;
    mt_wr16(ip + 10, 0);
    mt_wr32(ip + 12, src);
    mt_wr32(ip + 16, dst);
    mt_wr16(ip + 10, mt_cksum_fold(mt_cksum_add(0, ip, 20)));
    if (plen) memcpy(ip + 20, pl, plen);
    return (uint16_t)(34 + plen);
}

int view_arp(const uint8_t *f, uint16_t len, arpv *o) {
    if (len < 42 || mt_rd16(f + 12) != 0x0806) return 0;
    memcpy(o->eth_dst, f, 6);
    memcpy(o->eth_src, f + 6, 6);
    o->op = mt_rd16(f + 20);
    memcpy(o->smac, f + 22, 6);
    o->sip = mt_rd32(f + 28);
    memcpy(o->tmac, f + 32, 6);
    o->tip = mt_rd32(f + 38);
    return 1;
}

int view_ip(const uint8_t *f, uint16_t len, ipv *o) {
    const uint8_t *ip = f + 14;
    uint16_t ihl, tot, ff;
    if (len < 34 || mt_rd16(f + 12) != 0x0800 || (ip[0] >> 4) != 4) return 0;
    ihl = (uint16_t)((ip[0] & 15) * 4);
    tot = mt_rd16(ip + 2);
    if (ihl < 20 || tot < ihl || 14 + tot > len) return 0;
    ff = mt_rd16(ip + 6);
    memcpy(o->eth_dst, f, 6);
    memcpy(o->eth_src, f + 6, 6);
    o->src = mt_rd32(ip + 12);
    o->dst = mt_rd32(ip + 16);
    o->proto = ip[9];
    o->id = mt_rd16(ip + 4);
    o->flags = (uint16_t)(ff & 0xE000u);
    o->frag = (uint16_t)(ff & 0x1FFFu);
    o->payload = ip + ihl;
    o->len = (uint16_t)(tot - ihl);
    return 1;
}

uint16_t mk_tcp(uint8_t *f, mt_ip4 src, uint16_t sport, mt_ip4 dst, uint16_t dport, uint32_t seq,
                uint32_t ack, uint8_t flags, uint16_t win, const uint8_t *opt, uint8_t optlen,
                const uint8_t *data, uint16_t dlen) {
    uint8_t seg[1500];
    uint16_t hl = (uint16_t)(20 + optlen), tl = (uint16_t)(hl + dlen);
    uint32_t sum;
    mt_wr16(seg, sport);
    mt_wr16(seg + 2, dport);
    mt_wr32(seg + 4, seq);
    mt_wr32(seg + 8, ack);
    seg[12] = (uint8_t)(hl << 2);
    seg[13] = flags;
    mt_wr16(seg + 14, win);
    mt_wr16(seg + 16, 0);
    mt_wr16(seg + 18, 0);
    if (optlen) memcpy(seg + 20, opt, optlen);
    if (dlen) memcpy(seg + hl, data, dlen);
    sum = (src >> 16) + (src & 0xFFFFu) + (dst >> 16) + (dst & 0xFFFFu) + 6u + tl;
    mt_wr16(seg + 16, mt_cksum_fold(mt_cksum_add(sum, seg, tl)));
    return mk_ip(f, src, dst, 6, 77, 0, seg, tl);
}

int view_tcp(const uint8_t *f, uint16_t len, tcpv *v) {
    ipv ip;
    const uint8_t *t;
    uint16_t hl, i;
    uint32_t sum;
    if (!view_ip(f, len, &ip) || ip.proto != 6 || ip.len < 20) return 0;
    t = ip.payload;
    hl = (uint16_t)((t[12] >> 4) << 2);
    if (hl < 20 || hl > ip.len) return 0;
    v->src = ip.src; v->dst = ip.dst;
    v->sport = mt_rd16(t); v->dport = mt_rd16(t + 2);
    v->seq = mt_rd32(t + 4); v->ack = mt_rd32(t + 8);
    v->flags = (uint8_t)(t[13] & 0x3F);
    v->win = mt_rd16(t + 14);
    v->data = t + hl;
    v->dlen = (uint16_t)(ip.len - hl);
    v->mss = 0;
    for (i = 20; i + 4 <= hl; ) {
        if (t[i] == 0) break;
        if (t[i] == 1) { i++; continue; }
        if (t[i + 1] < 2) break;
        if (t[i] == 2 && t[i + 1] == 4) v->mss = mt_rd16(t + i + 2);
        i = (uint16_t)(i + t[i + 1]);
    }
    sum = (ip.src >> 16) + (ip.src & 0xFFFFu) + (ip.dst >> 16) + (ip.dst & 0xFFFFu) + 6u + ip.len;
    v->checksum_ok = mt_cksum_fold(mt_cksum_add(sum, t, ip.len)) == 0;
    return 1;
}

/* Segment arrays for tsend: three per TCB (at most MT_SENDQ_MAX are referenced by queued
 * requests at any time, so one is always free), keyed by the TCB address. */
static struct { const mt_tcp *t; mt_seg s[MT_SENDQ_MAX + 1][MT_SEGS_MAX]; } g_segpool[64];
static unsigned g_segnext;

static mt_seg *seg_slot(const mt_tcp *t) {
    unsigned i, k, r;
    for (i = 0; i < 64; i++)
        if (g_segpool[i].t == t) break;
    if (i == 64) {
        i = g_segnext++ & 63u;
        g_segpool[i].t = t;
    }
    for (k = 0; k <= MT_SENDQ_MAX; k++) {
        int used = 0;
        for (r = 0; r < t->sq_n; r++) used |= t->sq[r].segs == g_segpool[i].s[k];
        if (!used) return g_segpool[i].s[k];
    }
    return NULL;
}

int32_t tsend_cb(mt_tcp *t, const uint8_t *p, uint32_t len, int push, int urgent,
                 mt_send_done_cb done, void *ud, uint32_t now) {
    mt_seg *sg = seg_slot(t);
    uint8_t n = 0;
    uint32_t off = 0;
    int r;
    if (!sg) return MT_ERR_BUSY;
    while (off < len && n < MT_SEGS_MAX) {
        uint32_t c = len - off > 65535u ? 65535u : len - off;
        sg[n].p = p + off;
        sg[n].len = (uint16_t)c;
        off += c;
        n++;
    }
    if (off < len) return MT_ERR_INVAL;
    if (n == 0) { sg[0].p = p; sg[0].len = 0; n = 1; }
    r = mt_tcp_send_segs(t, sg, n, push, urgent, done, ud, now);
    return r == MT_OK ? (int32_t)len : r;
}

int32_t tsend(mt_tcp *t, const uint8_t *p, uint32_t len, int push, int urgent, uint32_t now) {
    return tsend_cb(t, p, len, push, urgent, NULL, NULL, now);
}

void rec_cb(mt_tcp *t, mt_tcp_event ev, uint32_t arg, void *user) {
    rec *r = (rec *)user;
    (void)t;
    if (r->n < 32) { r->e[r->n].ev = (int)ev; r->e[r->n].arg = arg; }
    r->n++;
}

mt_tcp *tconn_established(mt_stack *s, fake_link *fl, rec *events, uint32_t *peer_seq,
                          uint32_t *my_seq) {
    static uint8_t bufs[8][8192];
    static int next;
    static const uint8_t mss_opt[4] = {2, 4, 0x05, 0xB4};
    uint8_t f[200];
    tcpv v;
    mt_tcp *t = mt_tcp_create(s, bufs[next++ & 7], 8192, rec_cb, events);
    uint16_t n;
    if (!t) return NULL;
    mt_arp_insert(s, T_PEER, T_PEER_MAC, 0);
    if (mt_tcp_open(t, T_PEER, 23, 0, 0) != MT_OK || fl->n != 1 || !view_tcp(fl->f[0], fl->len[0], &v))
        return NULL;
    n = mk_tcp(f, T_PEER, 23, T_ME, v.sport, 5000, v.seq + 1, TF_SYN | TF_ACK, 8192, mss_opt, 4,
               NULL, 0);
    mt_input(s, f, n, 0);
    *peer_seq = 5001;
    *my_seq = v.seq + 1;
    fake_clear(fl);
    return t;
}
