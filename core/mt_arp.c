#include <string.h>
#include "mt_int.h"

#define ARP_RESOLVED_MS 600000u   /* 10 min */
#define ARP_PENDING_MS  3000u     /* give up after 3 s */
#define ARP_RETRY_MS    1000u     /* requests at +0, +1 s, +2 s */
#define ARP_MAX_TRIES   3u
#define ARP_LEN         28u
#define ARP_FRAME_LEN   60u       /* padded to the Ethernet minimum (arpbuf tail stays zero) */

static mt_arp_entry *arp_find(const mt_stack *s, mt_ip4 ip) {
    mt_arp_entry *e = s->arp, *end = s->arp + s->lim.arp_entries;
    for (; e < end; e++)
        if (e->state != MT_ARP_FREE && e->ip == ip) return e;
    return NULL;
}

static void arp_discard_held(mt_stack *s, mt_ip4 ip) {
    if (s->pend->len && s->pend->nexthop == ip) {
        s->pend->len = 0;
        s->stats.arp_drops++;
        MT_LOG_DROPPED(s, MT_LOGL_ARP, __LINE__);
    }
}

/* A pending entry gave up (or was evicted): count it once, along with its held frame. */
static void arp_drop_pending(mt_stack *s, mt_arp_entry *e) {
    int held = s->pend->len && s->pend->nexthop == e->ip;
    if (held) s->pend->len = 0;
    s->stats.arp_drops++;
    MT_LOG_DROPPED(s, MT_LOGL_ARP, __LINE__);
    e->state = MT_ARP_FREE;
}

/* Frees e if it timed out; returns 1 when it did. */
static int arp_expired(mt_stack *s, mt_arp_entry *e, uint32_t now) {
    if (e->state == MT_ARP_PENDING && !mt_time_before(now, e->t_stamp + ARP_PENDING_MS)) {
        mt_log(s, MT_LOG_ARP_FAIL, e->tries, (uint16_t)e->ip);
        arp_drop_pending(s, e);
        return 1;
    }
    if (e->state == MT_ARP_RESOLVED && !mt_time_before(now, e->t_stamp + ARP_RESOLVED_MS)) {
        e->state = MT_ARP_FREE;
        return 1;
    }
    return 0;
}

/* Free slot, else the least recently used entry (its held frame goes with it). */
static mt_arp_entry *arp_alloc(mt_stack *s) {
    mt_arp_entry *e = s->arp, *end = s->arp + s->lim.arp_entries, *victim = NULL;
    for (; e < end; e++) {
        if (e->state == MT_ARP_FREE) return e;
        if (!victim || mt_time_before(e->t_used, victim->t_used)) victim = e;
    }
    if (!victim) return NULL;
    if (victim->state == MT_ARP_PENDING) arp_drop_pending(s, victim);
    return victim;
}

/* Build and send an ARP packet from arpbuf. tmac is the target hardware address. */
static void arp_send(mt_stack *s, uint16_t op, const uint8_t *eth_dst, const uint8_t *tmac,
                     mt_ip4 tip) {
    uint8_t *b = s->arpbuf;
    if (s->cfg.addr == 0) return;
    memcpy(b, eth_dst, 6);
    memcpy(b + 6, s->cfg.mac, 6);
    mt_wr16(b + 12, 0x0806);
    mt_wr16(b + 14, 1);
    mt_wr16(b + 16, 0x0800);
    b[18] = 6;
    b[19] = 4;
    mt_wr16(b + 20, op);
    memcpy(b + 22, s->cfg.mac, 6);
    mt_wr32(b + 28, s->cfg.addr);
    memcpy(b + 32, tmac, 6);
    mt_wr32(b + 38, tip);
    if (mt_eth_tx(s, b, ARP_FRAME_LEN) == 0) s->stats.arp_out++;
}

/* Send the next request of a pending entry if it is due (at most one per ARP_RETRY_MS). */
static void arp_pending_step(mt_stack *s, mt_arp_entry *e, uint32_t now) {
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
    if (e->tries < ARP_MAX_TRIES &&
        !mt_time_before(now, e->t_stamp + (uint32_t)e->tries * ARP_RETRY_MS)) {
        e->tries++;
        arp_send(s, 1, bcast, zero, e->ip);
    }
}

int mt_arp_resolve(mt_stack *s, mt_ip4 nexthop, uint8_t mac_out[6], uint32_t now) {
    mt_arp_entry *e;
    if (s->cfg.addr == 0) return 0;
    e = arp_find(s, nexthop);
    if (e && arp_expired(s, e, now)) e = NULL;
    if (e && e->state == MT_ARP_RESOLVED) {
        memcpy(mac_out, e->mac, 6);
        e->t_used = now;
        return 1;
    }
    if (!e) {
        e = arp_alloc(s);
        if (!e) return 0;
        e->ip = nexthop;
        e->state = MT_ARP_PENDING;
        e->t_stamp = now;
        e->tries = 0;
    }
    e->t_used = now;
    arp_pending_step(s, e, now);
    return 0;
}

void mt_arp_insert(mt_stack *s, mt_ip4 ip, const uint8_t mac[6], uint32_t now) {
    mt_arp_entry *e = arp_find(s, ip);
    int was_pending;
    if (!e) e = arp_alloc(s);
    if (!e) return;
    was_pending = e->state == MT_ARP_PENDING;
    e->ip = ip;
    memcpy(e->mac, mac, 6);
    e->state = MT_ARP_RESOLVED;
    e->t_stamp = now;
    e->t_used = now;
    e->tries = 0;
    if (s->pend->len && s->pend->nexthop == ip) {
        mt_pending *p = s->pend;
        uint16_t n = p->len;
        memcpy(p->frame, mac, 6);
        p->len = 0;                       /* release the slot before tx */
        mt_tcp_note_released(s, p->frame + MT_ETH_HDR);   /* so TCP does not resend it */
        if (mt_eth_tx(s, p->frame, n) != 0) s->arp_rel.live = 0;   /* refused: TCP resends it */
    }
    if (was_pending) mt_tcp_arp_resolved(s, ip, now);
    s->arp_rel.live = 0;
}

void mt_arp_input(mt_stack *s, const uint8_t *arp, uint16_t len, uint32_t now) {
    uint16_t op;
    mt_ip4 spa;
    int known;
    s->stats.arp_in++;
    if (len < ARP_LEN || mt_rd16(arp) != 1 || mt_rd16(arp + 2) != 0x0800 ||
        arp[4] != 6 || arp[5] != 4)
        return;
    op = mt_rd16(arp + 6);
    if ((op != 1 && op != 2) || s->cfg.addr == 0) return;
    /* A multicast/broadcast sender hardware address is never valid, and our own MAC means a
     * reflection or a duplicate: ignore the packet entirely. */
    if ((arp[8] & 1u) || memcmp(arp + 8, s->cfg.mac, 6) == 0) return;
    spa = mt_rd32(arp + 14);
    /* RFC 826 merge: an existing entry (resolved or pending) for the sender is refreshed from
     * any well-formed ARP, whoever it is addressed to, so a changed gateway MAC is followed. A new
     * entry is only learned from a packet addressed to us. */
    known = spa != 0 && spa < 0xE0000000u && spa != s->cfg.addr && arp_find(s, spa) != NULL;
    if (known) {                       /* refresh MAC and lifetime only; t_used (LRU) is not touched */
        uint32_t used = arp_find(s, spa)->t_used;
        mt_arp_insert(s, spa, arp + 8, now);
        arp_find(s, spa)->t_used = used;
    }
    if (mt_rd32(arp + 24) != s->cfg.addr) return;
    if (!known && spa != 0 && spa < 0xE0000000u && spa != s->cfg.addr)
        mt_arp_insert(s, spa, arp + 8, now);
    if (op == 1) arp_send(s, 2, arp + 8, arp + 8, spa);
}

void mt_arp_tick(mt_stack *s, uint32_t now) {
    mt_arp_entry *e = s->arp, *end = s->arp + s->lim.arp_entries;
    for (; e < end; e++) {
        if (e->state == MT_ARP_FREE) continue;
        /* Resolved entries are invalidated here too, so a stale t_stamp/t_used
         * is never compared across a half clock wrap (tick at least every 2^31 ms). */
        if (!arp_expired(s, e, now) && e->state == MT_ARP_PENDING) arp_pending_step(s, e, now);
    }
    /* A held frame whose next hop has no pending entry can never be released. */
    if (s->pend->len) {
        const mt_arp_entry *h = arp_find(s, s->pend->nexthop);
        if (!h || h->state != MT_ARP_PENDING) arp_discard_held(s, s->pend->nexthop);
    }
}

uint32_t mt_arp_deadline(const mt_stack *s, uint32_t now) {
    const mt_arp_entry *e = s->arp, *end = s->arp + s->lim.arp_entries;
    uint32_t best = MT_NO_DEADLINE;
    for (; e < end; e++) {
        uint32_t at, rel;
        if (e->state == MT_ARP_FREE) continue;
        if (e->state == MT_ARP_RESOLVED)
            at = e->t_stamp + ARP_RESOLVED_MS;
        else
            at = e->t_stamp + (e->tries < ARP_MAX_TRIES ? (uint32_t)e->tries * ARP_RETRY_MS
                                                        : ARP_PENDING_MS);
        rel = mt_time_before(at, now) ? 0u : at - now;
        if (rel < best) best = rel;
    }
    return best;
}
