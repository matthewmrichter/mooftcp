#include <string.h>
#include "net.h"
#include "mt_util.h"

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
    static uint32_t pool[4][16384 / 4];
    static int next;
    mt_limits lim;
    mt_link link;
    mt_ifconfig c;
    mt_stack *s;
    uint32_t *mem = pool[next++ & 3];
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
