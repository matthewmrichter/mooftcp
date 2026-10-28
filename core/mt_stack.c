#include <string.h>
#include "mt_int.h"

/* ---- Arena layout: the single source of truth for size accounting ---- */

typedef struct {
    uint32_t arp;       /* offset of the ARP table */
    uint32_t pend;      /* offset of the pending-frame slot */
    uint32_t total;     /* bytes needed, including struct mt_stack at offset 0 */
} mt_layout;

static void mt_layout_calc(const mt_limits *l, mt_layout *o) {
    uint32_t off = MT_ALIGN4((uint32_t)sizeof(struct mt_stack));
    o->arp = off;
    off += MT_ALIGN4((uint32_t)l->arp_entries * (uint32_t)sizeof(mt_arp_entry));
    o->pend = off;
    off += MT_ALIGN4((uint32_t)sizeof(mt_pending));
    /* Later tasks: add their section here (offset = off; off += MT_ALIGN4(bytes);). */
    o->total = off;
}

void mt_limits_default(mt_limits *l) {
    l->max_tcp = 16;
    l->max_udp = 8;
    l->arp_entries = 8;
    l->dns_cache = 8;
}

uint32_t mt_stack_size(const mt_limits *l) {
    mt_layout lay;
    mt_layout_calc(l, &lay);
    return lay.total;
}

mt_stack *mt_stack_init(void *mem, uint32_t len, const mt_limits *l, const mt_link *link,
                        uint32_t seed) {
    mt_layout lay;
    mt_stack *s;
    uint8_t *base = (uint8_t *)mem;
    if (!mem || !l || !link || ((uintptr_t)mem & 3u)) return NULL;
    mt_layout_calc(l, &lay);
    if (len < lay.total) return NULL;
    memset(mem, 0, lay.total);
    s = (mt_stack *)mem;
    s->link = *link;
    s->lim = *l;
    s->seed = seed;
    s->arp = (mt_arp_entry *)(void *)(base + lay.arp);
    s->pend = (mt_pending *)(void *)(base + lay.pend);
    /* Later tasks: point their sections here. */
    return s;
}

void mt_set_config(mt_stack *s, const mt_ifconfig *c) { s->cfg = *c; }
void mt_get_config(const mt_stack *s, mt_ifconfig *c) { *c = s->cfg; }
const mt_stats *mt_get_stats(const mt_stack *s) { return &s->stats; }

/* ---- Ethernet ---- */

int mt_eth_tx(mt_stack *s, const uint8_t *frame, uint16_t len) {
    if (!s->link.tx || s->link.tx(s->link.ctx, frame, len) != 0) {
        s->stats.eth_tx_err++;
        return -1;
    }
    s->stats.eth_out++;
    return 0;
}

void mt_input(mt_stack *s, const uint8_t *frame, uint16_t len, uint32_t now) {
    uint16_t type;
    if (len < MT_ETH_HDR) return;
    if (memcmp(frame, s->cfg.mac, 6) != 0 &&
        (frame[0] & frame[1] & frame[2] & frame[3] & frame[4] & frame[5]) != 0xFF)
        return;
    s->stats.eth_in++;
    type = mt_rd16(frame + 12);
    if (type == 0x0806) mt_arp_input(s, frame + MT_ETH_HDR, (uint16_t)(len - MT_ETH_HDR), now);
    else if (type == 0x0800) mt_ip_input(s, frame + MT_ETH_HDR, (uint16_t)(len - MT_ETH_HDR), now);
}

int mt_eth_send(mt_stack *s, mt_ip4 nexthop, uint16_t ethertype, uint16_t payload_len,
                uint32_t now) {
    uint8_t mac[6];
    if (s->cfg.addr == 0 || payload_len > MT_TXBUF_SIZE - MT_ETH_HDR) return -1;
    if (nexthop == MT_ETH_BCAST_IP) {
        memset(mac, 0xFF, 6);
    } else if (!mt_arp_resolve(s, nexthop, mac, now)) {
        mt_pending *p = s->pend;
        if (p->len) s->stats.arp_drops++;          /* older held frame replaced */
        memcpy(p->frame + 6, s->cfg.mac, 6);
        mt_wr16(p->frame + 12, ethertype);
        memcpy(p->frame + MT_ETH_HDR, s->txbuf + MT_ETH_HDR, payload_len);
        p->nexthop = nexthop;
        p->len = (uint16_t)(MT_ETH_HDR + payload_len);
        return 0;
    }
    memcpy(s->txbuf, mac, 6);
    memcpy(s->txbuf + 6, s->cfg.mac, 6);
    mt_wr16(s->txbuf + 12, ethertype);
    return mt_eth_tx(s, s->txbuf, (uint16_t)(MT_ETH_HDR + payload_len)) == 0 ? 1 : -1;
}

/* ---- Timers: plain call chain; see the module checklist in mt_int.h ---- */

void mt_tick(mt_stack *s, uint32_t now) {
    mt_arp_tick(s, now);
    /* Later: mt_ip_timers (reassembly), mt_tcp_timers, mt_dns_timers. */
}

uint32_t mt_next_deadline(const mt_stack *s, uint32_t now) {
    uint32_t d = mt_arp_deadline(s, now);
    /* Later: d = mt_dl_min(d, mt_tcp_deadline(s, now)); likewise reassembly, DNS. */
    return d;
}
