#include <string.h>
#include "mt_int.h"

/* DHCP client (RFC 2131 / 2132). Design and rulings: docs/dev/DHCP-DESIGN.md.
 *
 * No endpoint, no receive buffer: replies arrive through mt_udp_input's no-endpoint branch (and,
 * while unconfigured, through the port-68 exception in ip_in) and are parsed in place. Messages
 * are built at mt_ip_payload(): with an address through mt_ip_output, without one through
 * mt_ip_output_bcast0 (source 0.0.0.0, Ethernet broadcast). The lease is applied straight into
 * s->cfg followed by mt_cfg_commit, so no mt_ifconfig copy lives on the (interrupt-time) stack. */

#define DH_HDR        236u          /* fixed BOOTP part */
#define DH_OPTS       240u          /* options start after the magic cookie */
#define DH_MAGIC      0x63825363u
#define DH_MIN        300u          /* BOOTP minimum message (RFC 1542 2.1) */
#define DH_CAP_S      1000000u      /* longest single wake-up, s (keeps now - t_base < 2^31 ms) */
#define DH_LEASE_DEF  3600u         /* lease when the ACK carries none */
#define DH_LEASE_MIN  20u
#define DH_REQ_TRIES  4u            /* REQUESTs in REQUESTING before a new DISCOVER round */
#define DH_NAK_MS     2000u         /* + 0..1000 ms: pause after a NAK */
#define DH_QUICK_MS   4000u         /* first resend when renewing (the first send primes ARP) */
#define DH_REXMIT_MIN 60000u        /* RENEWING / REBINDING resends: at least 60 s (RFC 2131 4.4.5) */

enum { DISCOVER = 1, OFFER, REQUEST, DECLINE, ACK, NAK };
enum { H_SID = 1, H_MASK = 2, H_ROUTER = 4, H_DNS = 8, H_LEASE = 16, H_T1 = 32, H_T2 = 64 };

typedef struct {
    const uint8_t *dom;
    mt_ip4 sid, mask, router, dns;
    uint32_t lease, t1, t2;
    uint8_t type, have, domlen;
} dh_opts;

static uint32_t dh_rnd(mt_dhcp *d) {
    uint32_t x = d->rnd;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    d->rnd = x;
    return x;
}

void mt_dhcp_reseed(mt_stack *s, uint32_t a, uint32_t b) {
    s->dhcp->rnd = (a ^ (b << 7 | b >> 25) ^ 0xD4C9E1A3u) | 1u;
}

/* 4, 8, 16, 32, 64, 64 ... s, each +-1 s (RFC 2131 4.1). */
static uint32_t dh_backoff(mt_dhcp *d) {
    uint32_t base = 4000u << (d->tries < 5u ? d->tries : 4u);
    return base - 1000u + dh_rnd(d) % 2001u;
}

static uint32_t dh_rel(uint32_t at, uint32_t now) {
    return mt_time_before(now, at) ? at - now : 0u;
}

/* ms from now until `rem` seconds after t_base (a wake-up at most DH_CAP_S away). */
static uint32_t dh_until(const mt_dhcp *d, uint32_t rem, uint32_t now) {
    uint32_t target, el;
    if (rem == MT_DHCP_INF) return MT_NO_DEADLINE;
    target = (rem > DH_CAP_S ? DH_CAP_S : rem) * 1000u;
    el = now - d->t_base;
    return target > el ? target - el : 0u;
}

static void dh_sub(uint32_t *v, uint32_t el) {
    if (*v != MT_DHCP_INF) *v = *v > el ? *v - el : 0u;
}

/* Move t_base forward by the whole seconds elapsed. */
static void dh_rebase(mt_dhcp *d, uint32_t now) {
    uint32_t el = (now - d->t_base) / 1000u;
    if (!el) return;
    d->t_base += el * 1000u;
    dh_sub(&d->t1, el);
    dh_sub(&d->t2, el);
    dh_sub(&d->left, el);
}

/* After a state change: one log record, then the embedder's callback (last: it may call the API). */
static void dh_notify(mt_stack *s, int ev) {
    mt_dhcp *d = s->dhcp;
    mt_log(s, MT_LOG_DHCP, d->state, (uint16_t)s->cfg.addr);
    if (d->cb)
        d->cb(d->user, ev, d->state, s->cfg.addr, d->state >= MT_DHCP_BOUND ? d->lease : 0u);
}

/* Build and send one message of `type` for the current state. */
static void dh_send(mt_stack *s, uint8_t type, uint32_t now) {
    mt_dhcp *d = s->dhcp;
    uint8_t *u = mt_ip_payload(s), *b = u + 8, *o = b + DH_OPTS;
    uint16_t n = 0, ulen;
    uint32_t el, sum;
    uint16_t c;
    mt_ip4 dst = d->state == MT_DHCP_RENEWING ? d->server : MT_ETH_BCAST_IP;
    mt_ip4 req = type == DISCOVER ? d->hint : d->state == MT_DHCP_REQUESTING ? d->offer : 0u;
    mt_ip4 src;
    int r;
    memset(b, 0, DH_MIN);
    b[0] = 1;                                       /* BOOTREQUEST, Ethernet, 6-byte MAC */
    b[1] = 1;
    b[2] = 6;
    mt_wr32(b + 4, d->xid);
    el = (now - d->t_round) / 1000u;
    mt_wr16(b + 8, (uint16_t)(el > 0xFFFFu ? 0xFFFFu : el));
    if (d->state == MT_DHCP_RENEWING || d->state == MT_DHCP_REBINDING) mt_wr32(b + 12, s->cfg.addr);
    memcpy(b + 28, s->cfg.mac, 6);
    mt_wr32(b + DH_HDR, DH_MAGIC);
    o[n++] = 53; o[n++] = 1; o[n++] = type;
    o[n++] = 61; o[n++] = 7; o[n++] = 1;            /* client id: hardware type + MAC */
    memcpy(o + n, s->cfg.mac, 6); n += 6;
    o[n++] = 57; o[n++] = 2; mt_wr16(o + n, 1500); n += 2;
    if (req) { o[n++] = 50; o[n++] = 4; mt_wr32(o + n, req); n += 4; }
    if (type == REQUEST && d->state == MT_DHCP_REQUESTING) {
        o[n++] = 54; o[n++] = 4; mt_wr32(o + n, d->server); n += 4;
    }
    o[n++] = 55; o[n++] = 7;
    o[n++] = 1; o[n++] = 3; o[n++] = 6; o[n++] = 15; o[n++] = 51; o[n++] = 58; o[n++] = 59;
    o[n++] = 255;
    n = (uint16_t)(DH_OPTS + n);
    if (n < DH_MIN) n = DH_MIN;
    ulen = (uint16_t)(8u + n);
    mt_wr16(u, 68);
    mt_wr16(u + 2, 67);
    mt_wr16(u + 4, ulen);
    mt_wr16(u + 6, 0);
    src = mt_ip_src_for(s, dst);
    sum = (src >> 16) + (src & 0xFFFFu) + (dst >> 16) + (dst & 0xFFFFu) + 17u + ulen;
    c = mt_cksum_fold(mt_cksum_add(sum, u, ulen));
    mt_wr16(u + 6, c ? c : 0xFFFFu);
    r = s->cfg.addr ? mt_ip_output(s, dst, 17, ulen, now) : mt_ip_output_bcast0(s, 17, ulen);
    if (r == MT_OK) s->stats.udp_out++;
    d->n_tx++;
}

/* INIT -> SELECTING: a new exchange, its first DISCOVER. */
static void dh_round(mt_stack *s, uint32_t now) {
    mt_dhcp *d = s->dhcp;
    d->xid = dh_rnd(d);
    d->t_round = now;
    d->tries = 0;
    d->offer = 0;
    d->server = 0;
    d->state = MT_DHCP_SELECTING;
    dh_send(s, DISCOVER, now);
    d->t_next = now + dh_backoff(d);
    dh_notify(s, MT_DHCP_EV_STATE);
}

/* Drop the leased address (RFC 2131: it MUST NOT be used after expiry or a NAK). 1 if one was held. */
static int dh_drop_address(mt_stack *s) {
    mt_ip4 oa = s->cfg.addr, om = s->cfg.mask, og = s->cfg.gateway;
    if (!oa) return 0;
    s->dhcp->hint = oa;                             /* ask for it again */
    s->cfg.addr = 0;
    mt_cfg_commit(s, oa, om, og, s->cfg.mtu);
    return 1;
}

/* Back to INIT, the next DISCOVER round after delay_ms; LOST first if an address was held. */
static void dh_restart(mt_stack *s, uint32_t delay_ms, uint32_t now) {
    mt_dhcp *d = s->dhcp;
    int lost = dh_drop_address(s);
    d->state = MT_DHCP_INIT;
    d->lease = 0;
    d->t_next = now + delay_ms;
    dh_notify(s, lost ? MT_DHCP_EV_LOST : MT_DHCP_EV_STATE);
}

static mt_ip4 dh_class_mask(mt_ip4 a) {
    return a < 0x80000000u ? 0xFF000000u : a < 0xC0000000u ? 0xFFFF0000u : 0xFFFFFF00u;
}

static int dh_mask_ok(mt_ip4 m) {
    mt_ip4 inv = ~m;
    return m != 0 && (inv & (inv + 1u)) == 0;
}

/* A unicast host address (the rule of mac/glue/mt_cfg.h mt_cfg_addr_valid). */
static int dh_addr_ok(mt_ip4 a, mt_ip4 mask) {
    mt_ip4 host = a & ~mask;
    if (a < 0x01000000u || a >= 0xE0000000u || mt_ip_is_127(a)) return 0;
    return !(~mask > 1u && (host == 0 || host == ~mask));
}

/* Walk the options; 1 if they end with the end option and carry a message type. */
static int dh_parse(const uint8_t *o, uint16_t n, dh_opts *p) {
    uint16_t i = 0;
    memset(p, 0, sizeof *p);
    while (i < n) {
        uint8_t c = o[i], l;
        const uint8_t *v;
        if (c == 0) { i++; continue; }
        if (c == 255) return p->type != 0;
        if ((uint32_t)i + 1u >= n) return 0;
        l = o[i + 1];
        if ((uint32_t)i + 2u + l > n) return 0;
        v = o + i + 2;
        switch (c) {
        case 53: if (l != 1 || p->type) return 0; p->type = v[0]; break;
        case 54: if (l == 4) { p->sid = mt_rd32(v); p->have |= H_SID; } break;
        case 1:  if (l == 4) { p->mask = mt_rd32(v); p->have |= H_MASK; } break;
        case 3:  if (l >= 4) { p->router = mt_rd32(v); p->have |= H_ROUTER; } break;
        case 6:  if (l >= 4) { p->dns = mt_rd32(v); p->have |= H_DNS; } break;
        case 51: if (l == 4) { p->lease = mt_rd32(v); p->have |= H_LEASE; } break;
        case 58: if (l == 4) { p->t1 = mt_rd32(v); p->have |= H_T1; } break;
        case 59: if (l == 4) { p->t2 = mt_rd32(v); p->have |= H_T2; } break;
        case 15: p->dom = v; p->domlen = l; break;
        default: break;
        }
        i = (uint16_t)(i + 2u + l);
    }
    return 0;                                       /* no end option */
}

/* Option 15 into cfg.domain: printable characters up to a NUL, at most 63; anything else leaves
 * the old domain. */
static void dh_domain(mt_stack *s, const uint8_t *v, uint8_t l) {
    uint8_t k, n = 0;
    while (n < l && v[n]) n++;
    if (n == 0) return;
    for (k = 0; k < n; k++)
        if (v[k] < 0x21u || v[k] > 0x7Eu) return;
    if (n > sizeof s->cfg.domain - 1u) n = (uint8_t)(sizeof s->cfg.domain - 1u);
    memcpy(s->cfg.domain, v, n);
    s->cfg.domain[n] = 0;
}

/* ACK: apply the lease, BOUND. */
static void dh_bind(mt_stack *s, mt_ip4 yi, mt_ip4 mask, const dh_opts *p) {
    mt_dhcp *d = s->dhcp;
    mt_ip4 oa = s->cfg.addr, om = s->cfg.mask, og = s->cfg.gateway;
    uint32_t lease = (p->have & H_LEASE) ? p->lease : DH_LEASE_DEF, t1, t2;
    if (lease < DH_LEASE_MIN) lease = DH_LEASE_MIN;
    if (lease == MT_DHCP_INF) {
        t1 = t2 = MT_DHCP_INF;
    } else {
        t1 = lease / 2u;
        t2 = lease - lease / 8u;
        if ((p->have & H_T1) && p->t1 < lease) t1 = p->t1;
        if ((p->have & H_T2) && p->t2 < lease && p->t2 >= t1) t2 = p->t2;
        if (t1 > t2) t1 = t2;
    }
    s->cfg.addr = yi;
    s->cfg.mask = mask;
    if ((p->have & H_ROUTER) && p->router) s->cfg.gateway = p->router;
    if ((p->have & H_DNS) && p->dns) s->cfg.dns = p->dns;
    if (p->dom) dh_domain(s, p->dom, p->domlen);
    mt_cfg_commit(s, oa, om, og, s->cfg.mtu);
    d->lease = lease;
    d->t1 = t1;
    d->t2 = t2;
    d->left = lease;
    d->t_base = d->t_sent;                          /* the lease runs from the REQUEST */
    d->offer = yi;
    if (p->have & H_SID) d->server = p->sid;
    d->tries = 0;
    d->n_ack++;
    d->state = MT_DHCP_BOUND;
    dh_notify(s, oa != yi ? MT_DHCP_EV_BOUND : MT_DHCP_EV_STATE);
}

void mt_dhcp_input(mt_stack *s, const uint8_t *b, uint16_t n, uint32_t now) {
    mt_dhcp *d = s->dhcp;
    dh_opts p;
    mt_ip4 yi, mask;
    uint16_t why;
    if (n < DH_OPTS || b[0] != 2 || b[1] != 1 || b[2] != 6 || mt_rd32(b + 4) != d->xid ||
        memcmp(b + 28, s->cfg.mac, 6) != 0 || mt_rd32(b + DH_HDR) != DH_MAGIC)
        MT_DROP_GOTO(why);
    if (!dh_parse(b + DH_OPTS, (uint16_t)(n - DH_OPTS), &p)) MT_DROP_GOTO(why);
    yi = mt_rd32(b + 16);
    mask = ((p.have & H_MASK) && dh_mask_ok(p.mask)) ? p.mask : dh_class_mask(yi);
    switch (p.type) {
    case OFFER:
        if (d->state != MT_DHCP_SELECTING) return;  /* late or duplicate: not an error */
        if (!(p.have & H_SID) || !dh_addr_ok(yi, mask)) MT_DROP_GOTO(why);
        d->n_offer++;
        d->offer = yi;
        d->server = p.sid;
        d->state = MT_DHCP_REQUESTING;
        d->tries = 0;
        d->t_sent = now;
        dh_send(s, REQUEST, now);
        d->t_next = now + dh_backoff(d);
        dh_notify(s, MT_DHCP_EV_STATE);
        return;
    case ACK:
    case NAK:
        if (d->state < MT_DHCP_REQUESTING || d->state == MT_DHCP_BOUND) return;
        if (d->state == MT_DHCP_REQUESTING && (p.have & H_SID) && p.sid != d->server) return;   /* not ours */
        if (p.type == NAK) {
            d->n_nak++;
            dh_restart(s, DH_NAK_MS + dh_rnd(d) % 1001u, now);
            return;
        }
        if (!dh_addr_ok(yi, mask)) MT_DROP_GOTO(why);
        dh_bind(s, yi, mask, &p);
        return;
    default:
        MT_DROP_GOTO(why);
    }
drop:
    d->n_bad++;
    MT_LOG_DROPPED(s, MT_LOGL_UDP, why);
}

void mt_dhcp_timers(mt_stack *s, uint32_t now) {
    mt_dhcp *d = s->dhcp;
    switch (d->state) {
    case MT_DHCP_OFF:
        return;
    case MT_DHCP_INIT:
        if (!mt_time_before(now, d->t_next)) dh_round(s, now);
        return;
    case MT_DHCP_SELECTING:
        if (mt_time_before(now, d->t_next)) return;
        if (d->tries < 255u) d->tries++;
        dh_send(s, DISCOVER, now);
        d->t_next = now + dh_backoff(d);
        return;
    case MT_DHCP_REQUESTING:
        if (mt_time_before(now, d->t_next)) return;
        if (d->tries + 1u >= DH_REQ_TRIES) {        /* no ACK: start over at once */
            dh_restart(s, 0, now);
            if (d->state == MT_DHCP_INIT) dh_round(s, now);
            return;
        }
        d->tries++;
        d->t_sent = now;
        dh_send(s, REQUEST, now);
        d->t_next = now + dh_backoff(d);
        return;
    default:                                        /* BOUND, RENEWING, REBINDING */
        break;
    }
    dh_rebase(d, now);
    if (d->left == 0) {                             /* the lease is over */
        dh_restart(s, 0, now);
        if (d->state == MT_DHCP_INIT) dh_round(s, now);
        return;
    }
    if (d->t2 == 0 && d->state != MT_DHCP_REBINDING) {
        d->state = MT_DHCP_REBINDING;               /* any server, broadcast */
    } else if (d->t1 == 0 && d->state == MT_DHCP_BOUND) {
        d->state = MT_DHCP_RENEWING;                /* our server, unicast */
        d->xid = dh_rnd(d);
        d->t_round = now;
        d->tries = 0;
    } else {
        if (d->state == MT_DHCP_BOUND || mt_time_before(now, d->t_next)) return;
        if (d->tries < 255u) d->tries++;
        d->t_sent = now;
        dh_send(s, REQUEST, now);
        d->t_next = now + (dh_until(d, d->state == MT_DHCP_RENEWING ? d->t2 : d->left, now) / 2u);
        if (mt_time_before(d->t_next, now + DH_REXMIT_MIN)) d->t_next = now + DH_REXMIT_MIN;
        return;
    }
    d->t_sent = now;                                /* entering RENEWING / REBINDING: send at once */
    dh_send(s, REQUEST, now);
    if (d->state == MT_DHCP_RENEWING) {
        d->t_next = now + DH_QUICK_MS;
    } else {
        d->t_next = now + dh_until(d, d->left, now) / 2u;
        if (mt_time_before(d->t_next, now + DH_REXMIT_MIN)) d->t_next = now + DH_REXMIT_MIN;
    }
    dh_notify(s, MT_DHCP_EV_STATE);
}

uint32_t mt_dhcp_deadline(const mt_stack *s, uint32_t now) {
    const mt_dhcp *d = s->dhcp;
    switch (d->state) {
    case MT_DHCP_OFF:        return MT_NO_DEADLINE;
    case MT_DHCP_BOUND:      return dh_until(d, d->t1, now);
    case MT_DHCP_RENEWING:   return mt_dl_min(dh_rel(d->t_next, now), dh_until(d, d->t2, now));
    case MT_DHCP_REBINDING:  return mt_dl_min(dh_rel(d->t_next, now), dh_until(d, d->left, now));
    default:                 return dh_rel(d->t_next, now);   /* INIT, SELECTING, REQUESTING */
    }
}

void mt_dhcp_set_cb(mt_stack *s, mt_dhcp_cb cb, void *user) {
    s->dhcp->cb = cb;
    s->dhcp->user = user;
}

int mt_dhcp_start(mt_stack *s, mt_ip4 hint, uint32_t now) {
    mt_dhcp *d = s->dhcp;
    static const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
    if (memcmp(s->cfg.mac, zero, 6) == 0) return MT_ERR_INVAL;
    (void)dh_drop_address(s);
    d->hint = hint;
    d->offer = 0;
    d->server = 0;
    d->lease = 0;
    d->tries = 0;
    d->state = MT_DHCP_INIT;
    d->t_next = now;
    dh_notify(s, MT_DHCP_EV_STATE);
    return MT_OK;
}

void mt_dhcp_stop(mt_stack *s) {
    s->dhcp->state = MT_DHCP_OFF;
    mt_log(s, MT_LOG_DHCP, MT_DHCP_OFF, (uint16_t)s->cfg.addr);
}

void mt_dhcp_get_info(const mt_stack *s, mt_dhcp_info *out, uint32_t now) {
    const mt_dhcp *d = s->dhcp;
    memset(out, 0, sizeof *out);
    out->state = d->state;
    out->addr = d->state >= MT_DHCP_BOUND ? s->cfg.addr : d->offer;
    out->server = d->server;
    if (d->state >= MT_DHCP_BOUND) {
        uint32_t el = (now - d->t_base) / 1000u;
        out->lease_s = d->lease;
        out->left_s = d->left == MT_DHCP_INF ? MT_DHCP_INF : d->left > el ? d->left - el : 0u;
    }
    out->tx = d->n_tx;
    out->offers = d->n_offer;
    out->acks = d->n_ack;
    out->naks = d->n_nak;
    out->bad = d->n_bad;
}
