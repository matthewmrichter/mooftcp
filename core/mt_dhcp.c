#include <string.h>
#include "mt_int.h"

/* DHCP client (RFC 2131 / 2132). Design and rulings: docs/dev/DHCP-DESIGN.md.
 *
 * No endpoint, no receive buffer: replies arrive through mt_udp_input's no-endpoint branch (and,
 * while unconfigured, through the port-68 exception in ip_in) and are parsed in place. Messages
 * are built at mt_ip_payload() and sent with mt_ip_output (while unconfigured it lets exactly
 * this through: UDP to 255.255.255.255 from 0.0.0.0, see mt_ip_output_hdr). The lease is applied
 * straight into s->cfg followed by mt_cfg_commit, so no mt_ifconfig copy lives on the
 * (interrupt-time) stack. Written for code size: the driver's code is resident on a 68000. */

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
#define T1   tm[0]
#define T2   tm[1]
#define LEFT tm[2]

/* The 32-bit options we read, by index into dh_opts.v (bit i of `have`): server id, mask, router,
 * DNS, lease, T1, T2. Router and DNS lists: the first address. Lengths: at least 4 (lenient). */
enum { V_SID, V_MASK, V_ROUTER, V_DNS, V_LEASE, V_T1, V_T2, V_N };
static const uint8_t dh_ids[V_N] = { 54, 1, 3, 6, 51, 58, 59 };
#define HAVE(p, i) (((p)->have >> (i)) & 1u)

/* Fixed options of every message: type (patched), client id (MAC patched), max message size 1500,
 * parameter request list: mask, router, DNS, domain, lease, T1, T2. */
static const uint8_t dh_tmpl[] = { 53, 1, 0, 61, 7, 1, 0, 0, 0, 0, 0, 0, 57, 2, 0x05, 0xDC,
                                   55, 7, 1, 3, 6, 15, 51, 58, 59 };

typedef struct {
    const uint8_t *dom;
    uint32_t v[V_N];
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
    return (4000u << (d->tries < 5u ? d->tries : 4u)) - 1000u + dh_rnd(d) % 2001u;
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

/* Move t_base forward by the whole seconds elapsed. */
static void dh_rebase(mt_dhcp *d, uint32_t now) {
    uint32_t el = (now - d->t_base) / 1000u;
    int i;
    if (!el) return;
    d->t_base += el * 1000u;
    for (i = 0; i < 3; i++)                         /* T1, T2, LEFT */
        if (d->tm[i] != MT_DHCP_INF) d->tm[i] = d->tm[i] > el ? d->tm[i] - el : 0u;
}

/* After a state change: one log record, then the embedder's callback (last: it may call the API). */
static void dh_notify(mt_stack *s, int ev) {
    mt_dhcp *d = s->dhcp;
    mt_log(s, MT_LOG_DHCP, d->state, (uint16_t)s->cfg.addr);
    if (d->cb)
        d->cb(d->user, ev, d->state, s->cfg.addr, d->state >= MT_DHCP_BOUND ? d->lease : 0u);
}

static uint16_t dh_opt4(uint8_t *o, uint8_t code, uint32_t v) {
    o[0] = code;
    o[1] = 4;
    mt_wr32(o + 2, v);
    return 6;
}

/* Build and send one message of `type` for the current state; DISCOVER and REQUEST only. */
static void dh_send(mt_stack *s, uint8_t type, uint32_t now) {
    mt_dhcp *d = s->dhcp;
    uint8_t *u = mt_ip_payload(s), *b = u + 8, *o = b + DH_OPTS;
    uint16_t n = sizeof dh_tmpl, ulen, c;
    uint32_t el, sum;
    int lease_held = d->state >= MT_DHCP_RENEWING;          /* RENEWING, REBINDING: ciaddr set */
    mt_ip4 dst = d->state == MT_DHCP_RENEWING ? d->server : MT_ETH_BCAST_IP, src;
    memset(b, 0, DH_MIN);
    b[0] = 1;                                       /* BOOTREQUEST, Ethernet, 6-byte MAC */
    b[1] = 1;
    b[2] = 6;
    mt_wr32(b + 4, d->xid);
    el = (now - d->t_round) / 1000u;
    mt_wr16(b + 8, (uint16_t)(el > 0xFFFFu ? 0xFFFFu : el));
    if (lease_held) mt_wr32(b + 12, s->cfg.addr);
    memcpy(b + 28, s->cfg.mac, 6);
    mt_wr32(b + DH_HDR, DH_MAGIC);
    memcpy(o, dh_tmpl, sizeof dh_tmpl);
    o[2] = type;
    memcpy(o + 6, s->cfg.mac, 6);
    if (type == DISCOVER) {
        if (d->hint) n = (uint16_t)(n + dh_opt4(o + n, 50, d->hint));
    } else if (!lease_held) {                       /* REQUEST answering an OFFER */
        n = (uint16_t)(n + dh_opt4(o + n, 50, d->offer));
        n = (uint16_t)(n + dh_opt4(o + n, 54, d->server));
    }
    o[n] = 255;
    ulen = 8u + DH_MIN;                             /* everything fits the 300-byte minimum */
    mt_wr16(u, 68);
    mt_wr16(u + 2, 67);
    mt_wr16(u + 4, ulen);
    mt_wr16(u + 6, 0);
    src = mt_ip_src_for(s, dst);
    sum = (src >> 16) + (src & 0xFFFFu) + (dst >> 16) + (dst & 0xFFFFu) + 17u + ulen;
    c = mt_cksum_fold(mt_cksum_add(sum, u, ulen));
    mt_wr16(u + 6, c ? c : 0xFFFFu);
    if (mt_ip_output(s, dst, 17, ulen, now) == MT_OK) s->stats.udp_out++;
    d->n_tx++;
}

/* Send a REQUEST now (its time is the lease start if it is answered) and set the next send. */
static void dh_request(mt_stack *s, uint32_t next_ms, uint32_t now) {
    mt_dhcp *d = s->dhcp;
    d->t_sent = now;
    dh_send(s, REQUEST, now);
    d->t_next = now + next_ms;
}

/* RENEWING / REBINDING: half the time left to T2 / to the end of the lease, at least 60 s. */
static uint32_t dh_rexmit(const mt_dhcp *d, uint32_t now) {
    uint32_t half = dh_until(d, d->state == MT_DHCP_RENEWING ? d->T2 : d->LEFT, now) / 2u;
    return half < DH_REXMIT_MIN ? DH_REXMIT_MIN : half;
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

/* Back to INIT, the next DISCOVER round after delay_ms. A leased address is dropped first (RFC
 * 2131: it MUST NOT be used after expiry or a NAK) and asked for again (hint); LOST then. */
static void dh_restart(mt_stack *s, uint32_t delay_ms, uint32_t now) {
    mt_dhcp *d = s->dhcp;
    mt_ip4 oa = s->cfg.addr;
    if (oa) {
        d->hint = oa;
        s->cfg.addr = 0;
        mt_cfg_commit(s, oa, s->cfg.mask, s->cfg.gateway, s->cfg.mtu);
    }
    d->state = MT_DHCP_INIT;
    d->lease = 0;
    d->t_next = now + delay_ms;
    dh_notify(s, oa ? MT_DHCP_EV_LOST : MT_DHCP_EV_STATE);
    if (!delay_ms && d->state == MT_DHCP_INIT) dh_round(s, now);
}

/* A unicast host address under mask (the rule of mac/glue/mt_cfg.h mt_cfg_addr_valid). */
static int dh_addr_ok(mt_ip4 a, mt_ip4 mask) {
    mt_ip4 host = a & ~mask;
    if (a < 0x01000000u || a >= 0xE0000000u || mt_ip_is_127(a)) return 0;
    return !(~mask > 1u && (host == 0 || host == ~mask));
}

/* Walk the options; 1 if they end with the end option and carry exactly one message type. */
static int dh_parse(const uint8_t *o, uint16_t n, dh_opts *p) {
    uint16_t i = 0;
    memset(p, 0, sizeof *p);
    while (i < n) {
        uint8_t c = o[i], l, k;
        const uint8_t *v;
        if (c == 0) { i++; continue; }
        if (c == 255) return p->type != 0;
        if ((uint32_t)i + 2u > n || (uint32_t)i + 2u + o[i + 1] > n) return 0;
        l = o[i + 1];
        v = o + i + 2;
        if (c == 53) {
            if (l != 1 || p->type) return 0;
            p->type = v[0];
        } else if (c == 15) {
            p->dom = v;
            p->domlen = l;
        } else if (l >= 4) {
            for (k = 0; k < V_N; k++)
                if (dh_ids[k] == c) { p->v[k] = mt_rd32(v); p->have |= (uint8_t)(1u << k); }
        }
        i = (uint16_t)(i + 2u + l);
    }
    return 0;                                       /* no end option */
}

/* ACK: apply the lease (fields the server sent; the rest keep the embedder's values), BOUND. */
static void dh_bind(mt_stack *s, mt_ip4 yi, mt_ip4 mask, const dh_opts *p) {
    mt_dhcp *d = s->dhcp;
    mt_ip4 oa = s->cfg.addr, om = s->cfg.mask, og = s->cfg.gateway;
    uint32_t lease = HAVE(p, V_LEASE) ? p->v[V_LEASE] : DH_LEASE_DEF, t1, t2;
    if (lease < DH_LEASE_MIN) lease = DH_LEASE_MIN;
    t1 = t2 = MT_DHCP_INF;
    if (lease != MT_DHCP_INF) {
        t1 = lease / 2u;
        t2 = lease - lease / 8u;
        if (HAVE(p, V_T1) && p->v[V_T1] < lease) t1 = p->v[V_T1];
        if (HAVE(p, V_T2) && p->v[V_T2] < lease && p->v[V_T2] >= t1) t2 = p->v[V_T2];
        if (t1 > t2) t1 = t2;
    }
    s->cfg.addr = yi;
    s->cfg.mask = mask;
    if (HAVE(p, V_ROUTER) && p->v[V_ROUTER]) s->cfg.gateway = p->v[V_ROUTER];
    if (HAVE(p, V_DNS) && p->v[V_DNS]) s->cfg.dns = p->v[V_DNS];
    if (p->dom) {                                   /* printable, up to a NUL, at most 63 */
        uint8_t k, n = 0;
        while (n < p->domlen && p->dom[n]) n++;
        for (k = 0; k < n && p->dom[k] > 0x20u && p->dom[k] < 0x7Fu; k++) {}
        if (n && k == n) {
            if (n > sizeof s->cfg.domain - 1u) n = (uint8_t)(sizeof s->cfg.domain - 1u);
            memcpy(s->cfg.domain, p->dom, n);
            s->cfg.domain[n] = 0;
        }
    }
    mt_cfg_commit(s, oa, om, og, s->cfg.mtu);
    d->lease = d->LEFT = lease;
    d->T1 = t1;
    d->T2 = t2;
    d->t_base = d->t_sent;                          /* the lease runs from the REQUEST */
    d->offer = yi;
    if (HAVE(p, V_SID)) d->server = p->v[V_SID];
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
    if (n < DH_OPTS || b[0] != 2 || mt_rd32(b + 4) != d->xid || memcmp(b + 28, s->cfg.mac, 6) != 0) {
        d->n_bad++;                                 /* another client's (LAN broadcast) or no BOOTP reply: */
        return;                                     /* counted, not logged (no log flooding) */
    }
    if (b[1] != 1 || b[2] != 6 || mt_rd32(b + DH_HDR) != DH_MAGIC ||
        !dh_parse(b + DH_OPTS, (uint16_t)(n - DH_OPTS), &p))
        MT_DROP_GOTO(why);
    yi = mt_rd32(b + 16);
    mask = p.v[V_MASK];
    if (!HAVE(&p, V_MASK) || !mask || ((~mask) & (~mask + 1u)))   /* absent or not contiguous */
        mask = yi < 0x80000000u ? 0xFF000000u : yi < 0xC0000000u ? 0xFFFF0000u : 0xFFFFFF00u;
    if (p.type == OFFER) {
        if (d->state != MT_DHCP_SELECTING) return;  /* late or duplicate: not an error */
        if (!HAVE(&p, V_SID) || !dh_addr_ok(yi, mask)) MT_DROP_GOTO(why);
        d->n_offer++;
        d->offer = yi;
        d->server = p.v[V_SID];
        d->state = MT_DHCP_REQUESTING;
        d->tries = 0;
        dh_request(s, dh_backoff(d), now);
        dh_notify(s, MT_DHCP_EV_STATE);
        return;
    }
    if (p.type != ACK && p.type != NAK) MT_DROP_GOTO(why);
    if (d->state < MT_DHCP_REQUESTING || d->state == MT_DHCP_BOUND) return;
    if (d->state == MT_DHCP_REQUESTING && HAVE(&p, V_SID) && p.v[V_SID] != d->server) return;   /* not ours */
    if (p.type == NAK) {
        d->n_nak++;
        dh_restart(s, DH_NAK_MS + dh_rnd(d) % 1001u, now);
        return;
    }
    if (!dh_addr_ok(yi, mask)) MT_DROP_GOTO(why);
    dh_bind(s, yi, mask, &p);
    return;
drop:
    d->n_bad++;
    MT_LOG_DROPPED(s, MT_LOGL_UDP, why);
}

void mt_dhcp_timers(mt_stack *s, uint32_t now) {
    mt_dhcp *d = s->dhcp;
    if (d->state == MT_DHCP_OFF) return;
    if (d->state <= MT_DHCP_REQUESTING) {           /* INIT, SELECTING, REQUESTING */
        if (mt_time_before(now, d->t_next)) return;
        if (d->state == MT_DHCP_INIT) {
            dh_round(s, now);
        } else if (d->state == MT_DHCP_SELECTING) {
            if (d->tries < 255u) d->tries++;
            dh_send(s, DISCOVER, now);
            d->t_next = now + dh_backoff(d);
        } else if (d->tries + 1u >= DH_REQ_TRIES) {
            dh_restart(s, 0, now);                  /* no ACK: start over at once */
        } else {
            d->tries++;
            dh_request(s, dh_backoff(d), now);
        }
        return;
    }
    dh_rebase(d, now);                              /* BOUND, RENEWING, REBINDING */
    if (d->LEFT == 0) {
        dh_restart(s, 0, now);                      /* the lease is over */
    } else if (d->T2 == 0 && d->state != MT_DHCP_REBINDING) {
        d->state = MT_DHCP_REBINDING;               /* any server, broadcast */
        dh_request(s, dh_rexmit(d, now), now);
        dh_notify(s, MT_DHCP_EV_STATE);
    } else if (d->T1 == 0 && d->state == MT_DHCP_BOUND) {
        d->state = MT_DHCP_RENEWING;                /* our server, unicast */
        d->xid = dh_rnd(d);
        d->t_round = now;
        d->tries = 0;
        dh_request(s, DH_QUICK_MS, now);
        dh_notify(s, MT_DHCP_EV_STATE);
    } else if (d->state != MT_DHCP_BOUND && !mt_time_before(now, d->t_next)) {
        if (d->tries < 255u) d->tries++;
        dh_request(s, dh_rexmit(d, now), now);
    }
}

uint32_t mt_dhcp_deadline(const mt_stack *s, uint32_t now) {
    const mt_dhcp *d = s->dhcp;
    switch (d->state) {
    case MT_DHCP_OFF:        return MT_NO_DEADLINE;
    case MT_DHCP_BOUND:      return dh_until(d, d->T1, now);
    case MT_DHCP_RENEWING:   return mt_dl_min(dh_rel(d->t_next, now), dh_until(d, d->T2, now));
    case MT_DHCP_REBINDING:  return mt_dl_min(dh_rel(d->t_next, now), dh_until(d, d->LEFT, now));
    default:                 return dh_rel(d->t_next, now);   /* INIT, SELECTING, REQUESTING */
    }
}

void mt_dhcp_set_cb(mt_stack *s, mt_dhcp_cb cb, void *user) {
    s->dhcp->cb = cb;
    s->dhcp->user = user;
}

int mt_dhcp_start(mt_stack *s, mt_ip4 hint, uint32_t now) {
    mt_dhcp *d = s->dhcp;
    mt_ip4 oa = s->cfg.addr;
    if (!(s->cfg.mac[0] | s->cfg.mac[1] | s->cfg.mac[2] | s->cfg.mac[3] | s->cfg.mac[4] | s->cfg.mac[5]))
        return MT_ERR_INVAL;
    s->now = now;                                   /* the log record below carries the time */
    if (oa) {
        s->cfg.addr = 0;
        mt_cfg_commit(s, oa, s->cfg.mask, s->cfg.gateway, s->cfg.mtu);
    }
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
        out->left_s = d->LEFT == MT_DHCP_INF ? MT_DHCP_INF : d->LEFT > el ? d->LEFT - el : 0u;
    }
    out->tx = d->n_tx;
    out->offers = d->n_offer;
    out->acks = d->n_ack;
    out->naks = d->n_nak;
    out->bad = d->n_bad;
}
