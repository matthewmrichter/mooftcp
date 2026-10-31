#include <string.h>
#include "mt_int.h"

/* ---- Arena layout: the single source of truth for size accounting ---- */

typedef struct {
    uint32_t arp;       /* offset of the ARP table */
    uint32_t pend;      /* offset of the pending-frame slot */
    uint32_t reasm;     /* offset of the IPv4 reassembly slot */
    uint32_t udp;       /* offset of the UDP endpoint array */
    uint32_t tcp;       /* offset of the TCB array */
    uint32_t dns;       /* offset of the DNS state (query slots + rx buffer) */
    uint32_t dcache;    /* offset of the DNS cache */
    uint32_t log;       /* offset of the diagnostic log ring */
    uint32_t total;     /* bytes needed, including struct mt_stack at offset 0 */
} mt_layout;

/* Every section starts at an MT_ARENA_ALIGN boundary (sections hold pointers). */
static uint32_t mt_arena_up(uint32_t x) { return (x + MT_ARENA_ALIGN - 1u) & ~(MT_ARENA_ALIGN - 1u); }

static void mt_layout_calc(const mt_limits *l, mt_layout *o) {
    uint32_t off = mt_arena_up((uint32_t)sizeof(struct mt_stack));
    o->arp = off;
    off = mt_arena_up(off + (uint32_t)l->arp_entries * (uint32_t)sizeof(mt_arp_entry));
    o->pend = off;
    off = mt_arena_up(off + (uint32_t)sizeof(mt_pending));
    o->reasm = off;
    off = mt_arena_up(off + (uint32_t)sizeof(mt_reasm));
    o->udp = off;
    off = mt_arena_up(off + ((uint32_t)l->max_udp + 1u) * (uint32_t)sizeof(mt_udp));
    o->tcp = off;
    off = mt_arena_up(off + (uint32_t)l->max_tcp * (uint32_t)sizeof(mt_tcp));
    o->dns = off;
    off = mt_arena_up(off + (uint32_t)sizeof(mt_dns));
    o->dcache = off;
    off = mt_arena_up(off + (uint32_t)l->dns_cache * (uint32_t)sizeof(mt_dns_cent));
    o->log = off;
    off = mt_arena_up(off + MT_LOG_RECS * (uint32_t)sizeof(mt_logrec));
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
    if (!mem || !l || !link || ((uintptr_t)mem & (MT_ARENA_ALIGN - 1u))) return NULL;
    mt_layout_calc(l, &lay);
    if (len < lay.total) return NULL;
    memset(mem, 0, lay.total);
    s = (mt_stack *)mem;
    s->link = *link;
    s->lim = *l;
    s->seed = seed;
    s->arp = (mt_arp_entry *)(void *)(base + lay.arp);
    s->pend = (mt_pending *)(void *)(base + lay.pend);
    s->reasm = (mt_reasm *)(void *)(base + lay.reasm);
    s->ip_id = (uint16_t)seed;
    s->udp = (mt_udp *)(void *)(base + lay.udp);
    s->udp_eph = (uint16_t)(49152u + seed % 16384u);
    s->tcp = (mt_tcp *)(void *)(base + lay.tcp);
    s->tcp_eph = (uint16_t)(49152u + (seed + 7919u) % 16384u);
    s->dns = (mt_dns *)(void *)(base + lay.dns);
    s->dns->rnd = (seed ^ 0x5DEECE66u) | 1u;
    s->lis_eph = (uint16_t)(1024u + (seed + 104729u) % 1024u);
    s->dcache = (mt_dns_cent *)(void *)(base + lay.dcache);
    s->log = (mt_logrec *)(void *)(base + lay.log);
    return s;
}

void mt_set_entropy(mt_stack *s, uint32_t a, uint32_t b) {
    s->seed = a;
    s->key = b ? b : 0x9E3779B9u;               /* nonzero: the tuple mix is active */
    s->ip_id = (uint16_t)(a ^ (b >> 16));
    s->udp_eph = (uint16_t)(49152u + (a ^ (b * 40503u)) % 16384u);
    s->tcp_eph = (uint16_t)(49152u + ((a + 7919u) ^ (b >> 3)) % 16384u);
    s->lis_eph = (uint16_t)(1024u + ((a >> 7) ^ b) % 1024u);
    mt_dns_reseed(s, a, b);
}

/* mtu is clamped to 68..1500 (IPv4 minimum, txbuf limit); 0 means 68, never "unlimited".
 * The full semantics are written down at the declaration in mt.h. */
void mt_set_config(mt_stack *s, const mt_ifconfig *c) {
    mt_ifconfig old = s->cfg;
    uint16_t i, lm;
    s->cfg = *c;
    if (s->cfg.mtu < 68u) s->cfg.mtu = 68u;
    if (s->cfg.mtu > MT_TXBUF_SIZE - MT_ETH_HDR) s->cfg.mtu = (uint16_t)(MT_TXBUF_SIZE - MT_ETH_HDR);
    if (old.addr != s->cfg.addr || old.mask != s->cfg.mask || old.gateway != s->cfg.gateway) {
        memset(s->arp, 0, (size_t)s->lim.arp_entries * sizeof(mt_arp_entry));
        if (s->pend->len) {                      /* the held frame was addressed from the old setup */
            s->pend->len = 0;
            s->stats.arp_drops++;
            MT_LOG_DROPPED(s, MT_LOGL_ARP, __LINE__);
        }
        s->arp_rel.live = 0;
    }
    if (s->cfg.mtu < old.mtu) {
        lm = mt_tcp_local_mss(s);
        for (i = 0; i < s->lim.max_tcp; i++)
            if (s->tcp[i].used && s->tcp[i].mss > lm) s->tcp[i].mss = lm;
    }
}
void mt_get_config(const mt_stack *s, mt_ifconfig *c) { *c = s->cfg; }
const mt_stats *mt_get_stats(const mt_stack *s) { return &s->stats; }

uint8_t mt_tcp_count_open(const mt_stack *s) {
    uint8_t i, n = 0;
    for (i = 0; i < s->lim.max_tcp; i++)
        if (s->tcp[i].used && s->tcp[i].state > MT_TCP_LISTEN) n++;
    return n;
}

/* ---- Diagnostic log ----
 * One writer context (whichever is inside the core), one reader context (mt_log_read), either
 * may interrupt the other. The writer fills slot (log_wr & 127), then publishes it by advancing
 * log_wr (one 16-bit store, after a barrier); it never touches log_rd. While it fills a slot,
 * log_wr still points at that slot, so a slot is stable only if it is not the one log_wr names:
 * of the 128 slots at most 127 are readable (wr - rd <= 127). The reader copies a record, then
 * re-reads log_wr (after a barrier) and keeps the copy only if the slot was still safe; otherwise
 * it skips ahead to the oldest safe record. A torn record is never returned. O(1) writes. */
void mt_log_put(mt_stack *s, uint8_t code, uint8_t a, uint16_t b) {
    uint16_t wr = s->log_wr;
    mt_logrec *r = &s->log[wr & (MT_LOG_RECS - 1u)];
    r->t = s->now;
#ifdef MT_TEST_HOOKS
    if (s->log_put_spy) s->log_put_spy(s);
#endif
    r->code = code;
    r->a = a;
    r->b = b;
    MT_BARRIER();                                       /* record complete before it is published */
    s->log_wr = (uint16_t)(wr + 1u);
}

void mt_log_enable(mt_stack *s, int on) { s->log_on = (uint8_t)(on != 0); }

uint16_t mt_log_read(mt_stack *s, mt_logrec *out, uint16_t max) {
    uint16_t n = 0;
    while (n < max) {
        uint16_t wr = s->log_wr, rd = s->log_rd;
        const mt_logrec *r;
        MT_BARRIER();                                   /* the copy below is after the log_wr load */
        if ((uint16_t)(wr - rd) >= MT_LOG_RECS) rd = (uint16_t)(wr - (MT_LOG_RECS - 1u));   /* lapped */
        if (rd == wr) {
            s->log_rd = rd;
            break;
        }
        r = &s->log[rd & (MT_LOG_RECS - 1u)];
        out[n].t = r->t;
        out[n].code = r->code;
        out[n].a = r->a;
        out[n].b = r->b;
        MT_BARRIER();                                   /* the copy is before the log_wr re-load */
        wr = s->log_wr;
        if ((uint16_t)(wr - rd) >= MT_LOG_RECS) {       /* writer reached this slot meanwhile: retry */
            s->log_rd = (uint16_t)(wr - (MT_LOG_RECS - 1u));
            continue;
        }
        s->log_rd = (uint16_t)(rd + 1u);
        n++;
    }
    return n;
}

/* ---- Ethernet ---- */

int mt_eth_tx(mt_stack *s, const uint8_t *frame, uint16_t len) {
    if (!s->link.tx || s->link.tx(s->link.ctx, frame, len) != 0) {
        s->stats.eth_tx_err++;
        MT_LOG_DROPPED(s, MT_LOGL_ETH, __LINE__);
        return -1;
    }
    s->stats.eth_out++;
    return 0;
}

void mt_input(mt_stack *s, const uint8_t *frame, uint16_t len, uint32_t now) {
    uint16_t type;
    s->now = now;
    if (len < MT_ETH_HDR) return;
    if (memcmp(frame, s->cfg.mac, 6) != 0 &&
        (frame[0] & frame[1] & frame[2] & frame[3] & frame[4] & frame[5]) != 0xFF)
        return;
    if (memcmp(frame + 6, s->cfg.mac, 6) == 0) return;      /* our own frame reflected back */
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
        if (MT_ETH_HDR + payload_len > MT_PEND_MAX) {
            s->stats.arp_drops++;                  /* too big to hold; request already sent */
            MT_LOG_DROPPED(s, MT_LOGL_ARP, __LINE__);
            return 0;
        }
        if (p->len) {                              /* older held frame replaced */
            s->stats.arp_drops++;
            MT_LOG_DROPPED(s, MT_LOGL_ARP, __LINE__);
        }
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
    if (mt_eth_tx(s, s->txbuf, (uint16_t)(MT_ETH_HDR + payload_len)) == 0) return 1;
    s->tx_refused = 1;                         /* the link is busy: TCP stalls and retries */
    return -1;
}

/* ---- Timers: plain call chain; see the module checklist in mt_int.h ---- */

void mt_tick(mt_stack *s, uint32_t now) {
    s->now = now;
    mt_arp_tick(s, now);
    mt_ip_timers(s, now);
    mt_tcp_timers(s, now);
    mt_dns_timers(s, now);
}

uint32_t mt_next_deadline(const mt_stack *s, uint32_t now) {
    uint32_t d = mt_arp_deadline(s, now);
    d = mt_dl_min(d, mt_ip_deadline(s, now));
    d = mt_dl_min(d, mt_tcp_deadline(s, now));
    d = mt_dl_min(d, mt_dns_deadline(s, now));
    return d;
}
