#include <string.h>
#include "mt_int.h"

/* TCP connection lifecycle and segment arrival (RFC 793 processing order, RFC 5961-style RST
 * and SYN handling). Segment output lives in mt_tcp_out.c.
 *
 * Events are deferred to the end of segment processing: any ACK/data/window work is done and
 * the reply segment sent before the callbacks run, so an app callback never sees a half
 * processed segment and the peer is answered first. TERMINATE paths fire inline and return. */

#define TCP_HDR 20u
#define TCP_MIN_BUF 4096u
#define TCP_EPH_BASE 49152u
#define TCP_TW_MS 2000u            /* TIME_WAIT: short, keeps TCBs available on a 16-slot machine */
#define TCP_ULP_DEFAULT_S 120u
#define TCP_MSS_DEFAULT 536u
#define TCP_MSS_MIN 64u

/* 16 TCBs must fit the 16 KB stack budget: ~140 bytes with 32-bit pointers. */
typedef char mt_tcp_size_ok[sizeof(struct mt_tcp) <= 300u ? 1 : -1];

enum { EV_EST = 1, EV_DATA = 2, EV_CLOSING = 4 };

typedef struct {
    const uint8_t *data;
    uint32_t seq, ack;
    uint16_t dlen, win, mss;
    uint8_t fl;
} seg;

static uint32_t rel(uint32_t at, uint32_t now) { return mt_time_before(at, now) ? 0u : at - now; }
static void tmr_set(mt_tcp *t, unsigned i, uint32_t at) {
    t->tmr[i] = at;
    t->tmask |= (uint8_t)(1u << i);
}
static void tmr_clr(mt_tcp *t, unsigned i) { t->tmask &= (uint8_t)~(1u << i); }

static void fire(mt_tcp *t, mt_tcp_event ev, uint32_t arg) {
    if (t->used && t->cb) t->cb(t, ev, arg, t->user);
}

static int is_sync(unsigned st) {      /* a SYN-ACK has been sent or received: RST is meaningful */
    return st == MT_TCP_SYN_RCVD || st == MT_TCP_ESTABLISHED || st == MT_TCP_FIN_WAIT_1 ||
           st == MT_TCP_FIN_WAIT_2 || st == MT_TCP_CLOSE_WAIT || st == MT_TCP_CLOSING ||
           st == MT_TCP_LAST_ACK;
}

static void tcp_term(mt_tcp *t, mt_term_reason r) {
    if (t->flags & MT_TF_EST) t->st->stats.tcp_conns_open--;
    t->flags &= MT_TF_ULPABORT;
    t->tmask = 0;
    t->state = MT_TCP_CLOSED;
    t->gen++;
    fire(t, MT_TCP_EV_TERMINATE, r);
}

/* Reset connection variables and rings for a new open/listen. */
static void conn_reset(mt_tcp *t) {
    mt_ring_init(&t->sndq, t->sndq.base, t->sndq.size);
    mt_ring_init(&t->rcvq, t->rcvq.base, t->rcvq.size);
    t->snd_una = t->snd_nxt = t->snd_wnd = t->rcv_nxt = 0;
    t->snd_wl1 = t->snd_wl2 = t->iss = t->irs = 0;
    t->tmask = 0;
    t->rto = MT_TCP_RTO_INIT;
    t->srtt = 0;
    t->flags &= MT_TF_ULPABORT;
    t->mss = TCP_MSS_DEFAULT;
}

static uint32_t isn(const mt_stack *s, uint32_t now) { return s->seed + now * 250u; }

static void set_mss(mt_tcp *t, uint16_t peer) {
    uint16_t m = peer >= TCP_MSS_MIN ? peer : (uint16_t)TCP_MSS_DEFAULT;
    uint16_t l = mt_tcp_local_mss(t->st);
    t->mss = m < l ? m : l;
}

static mt_ip4 nexthop(const mt_stack *s, mt_ip4 rip) {
    return ((rip ^ s->cfg.addr) & s->cfg.mask) == 0 ? rip : s->cfg.gateway;
}

/* ---- API ---- */

mt_tcp *mt_tcp_create(mt_stack *s, uint8_t *buf, uint32_t buflen, mt_tcp_cb cb, void *user) {
    mt_tcp *t = NULL;
    uint16_t i;
    uint32_t sq;
    uint8_t gen;
    if (!buf || buflen < TCP_MIN_BUF) return NULL;
    for (i = 0; i < s->lim.max_tcp; i++)
        if (!s->tcp[i].used) { t = &s->tcp[i]; break; }
    if (!t) return NULL;
    gen = (uint8_t)(t->gen + 1u);
    memset(t, 0, sizeof *t);
    t->gen = gen;
    sq = buflen / 4u;
    mt_ring_init(&t->sndq, buf, sq);
    mt_ring_init(&t->rcvq, buf + sq, buflen - sq);
    t->st = s;
    t->cb = cb;
    t->user = user;
    t->used = 1;
    t->ulp_s = TCP_ULP_DEFAULT_S;
    t->flags = MT_TF_ULPABORT;
    t->rto = MT_TCP_RTO_INIT;
    t->mss = TCP_MSS_DEFAULT;
    t->state = MT_TCP_CLOSED;
    return t;
}

void mt_tcp_set_ulp_timeout(mt_tcp *t, uint16_t seconds, int abort) {
    t->ulp_s = seconds;
    if (abort) t->flags |= MT_TF_ULPABORT;
    else t->flags &= (uint8_t)~MT_TF_ULPABORT;
}

static int port_busy(const mt_stack *s, const mt_tcp *self, uint16_t p) {
    uint16_t i;
    for (i = 0; i < s->lim.max_tcp; i++) {
        const mt_tcp *o = &s->tcp[i];
        if (o != self && o->used && o->state != MT_TCP_CLOSED && o->lport == p) return 1;
    }
    return 0;
}

int mt_tcp_open(mt_tcp *t, mt_ip4 rip, uint16_t rport, uint16_t lport, uint32_t now) {
    mt_stack *s = t->st;
    uint16_t i;
    if (t->state != MT_TCP_CLOSED) return MT_ERR_STATE;
    if (s->cfg.addr == 0) return MT_ERR_UNCONFIGURED;
    if (rip == 0 || rip == MT_ETH_BCAST_IP || rip == (s->cfg.addr | ~s->cfg.mask) || rport == 0)
        return MT_ERR_INVAL;
    if (nexthop(s, rip) == 0) return MT_ERR_NOROUTE;
    if (lport == 0) {
        uint32_t tries;
        uint16_t p = s->tcp_eph;
        for (tries = 0; tries < 65536u - TCP_EPH_BASE; tries++) {
            if (p < TCP_EPH_BASE) p = TCP_EPH_BASE;
            if (!port_busy(s, t, p)) break;
            p = (uint16_t)(p == 65535u ? TCP_EPH_BASE : p + 1u);
        }
        if (tries == 65536u - TCP_EPH_BASE) return MT_ERR_INUSE;
        lport = p;
        s->tcp_eph = (uint16_t)(p == 65535u ? TCP_EPH_BASE : p + 1u);
    } else {
        for (i = 0; i < s->lim.max_tcp; i++) {
            const mt_tcp *o = &s->tcp[i];
            if (o != t && o->used && o->state != MT_TCP_CLOSED && o->state != MT_TCP_LISTEN &&
                o->lport == lport && o->rport == rport && o->rip == rip)
                return MT_ERR_INUSE;
        }
    }
    conn_reset(t);
    t->rip = rip;
    t->rport = rport;
    t->lport = lport;
    t->flport = t->frport = 0;
    t->frip = 0;
    t->tnow = now;
    t->iss = isn(s, now);
    t->snd_una = t->snd_nxt = t->iss;      /* SYN goes out below, advancing snd_nxt */
    t->state = MT_TCP_SYN_SENT;
    s->stats.tcp_active_opens++;
    mt_tcp_output(t, now);
    return MT_OK;
}

int mt_tcp_listen(mt_tcp *t, mt_ip4 rip_filter, uint16_t rport_filter, uint16_t lport) {
    if (t->state != MT_TCP_CLOSED) return MT_ERR_STATE;
    conn_reset(t);
    t->frip = rip_filter;
    t->frport = rport_filter;
    t->flport = lport;
    t->rip = 0;
    t->rport = 0;
    t->lport = lport;
    t->state = MT_TCP_LISTEN;
    return MT_OK;
}

/* RST for a synchronized connection. */
static void send_rst(mt_tcp *t, uint32_t now) {
    if (is_sync(t->state))
        mt_tcp_send_raw(t->st, t->rip, t->lport, t->rport, t->snd_nxt, t->rcv_nxt,
                        MT_TH_RST | MT_TH_ACK, now);
}

static void back_to_listen(mt_tcp *t) {
    t->tmask = 0;
    t->flags &= MT_TF_ULPABORT;
    t->state = MT_TCP_LISTEN;
    t->rip = 0;
    t->rport = 0;
    t->lport = t->flport;
}

int mt_tcp_close(mt_tcp *t, uint32_t now) {
    t->tnow = now;
    switch (t->state) {
    case MT_TCP_LISTEN:
        t->state = MT_TCP_CLOSED;
        return MT_OK;
    case MT_TCP_SYN_SENT:
        tcp_term(t, MT_TERM_ULP_CLOSE);
        return MT_OK;
    case MT_TCP_SYN_RCVD:
        send_rst(t, now);
        tcp_term(t, MT_TERM_ULP_CLOSE);
        return MT_OK;
    case MT_TCP_ESTABLISHED:
        t->state = MT_TCP_FIN_WAIT_1;
        break;
    case MT_TCP_CLOSE_WAIT:
        t->state = MT_TCP_LAST_ACK;
        break;
    default:
        return MT_ERR_STATE;
    }
    t->flags |= MT_TF_FINQ;
    mt_tcp_output(t, now);
    return MT_OK;
}

void mt_tcp_abort(mt_tcp *t, uint32_t now) {
    t->tnow = now;
    if (t->state == MT_TCP_LISTEN) { t->state = MT_TCP_CLOSED; return; }
    if (t->state == MT_TCP_CLOSED) return;
    send_rst(t, now);
    tcp_term(t, MT_TERM_ULP_ABORT);
}

void mt_tcp_release(mt_tcp *t) {
    if (!t->used) return;
    send_rst(t, t->tnow);
    if (t->flags & MT_TF_EST) t->st->stats.tcp_conns_open--;
    {
        uint8_t gen = (uint8_t)(t->gen + 1u);
        memset(t, 0, sizeof *t);           /* used = 0, cb = NULL: no further callbacks */
        t->gen = gen;
    }
}

void mt_tcp_get_info(const mt_tcp *t, mt_tcp_info *i) {
    uint32_t sent = 0;
    memset(i, 0, sizeof *i);
    i->state = (mt_tcp_state)t->state;
    i->lip = t->st->cfg.addr;
    i->rip = t->rip;
    i->lport = t->lport;
    i->rport = t->rport;
    if (t->state >= MT_TCP_ESTABLISHED && t->state != MT_TCP_TIME_WAIT) {
        sent = t->snd_nxt - t->snd_una - ((t->flags & MT_TF_FINSENT) ? 1u : 0u);
        i->unacked = sent;
        i->send_queued = t->sndq.used > sent ? t->sndq.used - sent : 0u;
    }
    i->rcv_avail = t->rcvq.used;
    i->snd_wnd = t->snd_wnd;
    i->rcv_wnd = mt_ring_free(&t->rcvq);
    i->srtt_ms = t->srtt;
    i->rto_ms = t->rto;
}

/* ---- ICMP ---- */

void mt_tcp_icmp_error(mt_stack *s, mt_ip4 dst, uint16_t lport, uint16_t rport, uint8_t type,
                       uint8_t code) {
    uint16_t i;
#ifdef MT_TEST_HOOKS
    if (s->icmp_err_spy) s->icmp_err_spy(s->icmp_err_ctx, 6, dst, lport, rport, type, code);
#endif
    for (i = 0; i < s->lim.max_tcp; i++) {
        mt_tcp *t = &s->tcp[i];
        if (t->used && t->state != MT_TCP_CLOSED && t->state != MT_TCP_LISTEN &&
            t->rip == dst && t->lport == lport && t->rport == rport)
            fire(t, MT_TCP_EV_ICMP, ((uint32_t)type << 8) | code);   /* reported, not aborted */
    }
}

/* ---- Timers ---- */

static void enter_time_wait(mt_tcp *t, uint32_t now) {
    t->state = MT_TCP_TIME_WAIT;
    tmr_clr(t, MT_TT_RTO);
    tmr_clr(t, MT_TT_DELACK);
    tmr_set(t, MT_TT_TW, now + TCP_TW_MS);
}

static uint32_t ulp_ms(const mt_tcp *t) { return (uint32_t)t->ulp_s * 1000u; }

void mt_tcp_timers(mt_stack *s, uint32_t now) {
    uint16_t i;
    for (i = 0; i < s->lim.max_tcp; i++) {
        mt_tcp *t = &s->tcp[i];
        if (!t->used || !t->tmask) continue;
        t->tnow = now;
        if ((t->tmask & (1u << MT_TT_TW)) && !mt_time_before(now, t->tmr[MT_TT_TW])) {
            tcp_term(t, MT_TERM_ULP_CLOSE);
            continue;
        }
        if ((t->tmask & (1u << MT_TT_DELACK)) && !mt_time_before(now, t->tmr[MT_TT_DELACK])) {
            t->flags |= MT_TF_ACKNOW;
            mt_tcp_output(t, now);
        }
        if (!(t->tmask & (1u << MT_TT_RTO))) continue;
        if (ulp_ms(t) && !mt_time_before(now, t->ulp_start + ulp_ms(t))) {   /* ULP timeout */
            if (t->flags & MT_TF_ULPABORT) {
                if (t->state == MT_TCP_SYN_SENT) {
                    tcp_term(t, MT_TERM_OPEN_FAILED);
                } else if (t->state == MT_TCP_SYN_RCVD && (t->flags & MT_TF_PASSIVE)) {
                    back_to_listen(t);
                } else {
                    send_rst(t, now);
                    tcp_term(t, MT_TERM_ULP_TIMEOUT);
                }
            } else {
                t->ulp_start = now;
                fire(t, MT_TCP_EV_ULP_TIMEOUT, 0);
            }
        } else if (!mt_time_before(now, t->tmr[MT_TT_RTO])) {
            mt_tcp_rexmit(t, now);
            t->rto = t->rto * 2u > MT_TCP_RTO_MAX ? MT_TCP_RTO_MAX : t->rto * 2u;
            tmr_set(t, MT_TT_RTO, now + t->rto);
        }
    }
}

uint32_t mt_tcp_deadline(const mt_stack *s, uint32_t now) {
    uint32_t best = MT_NO_DEADLINE;
    uint16_t i;
    for (i = 0; i < s->lim.max_tcp; i++) {
        const mt_tcp *t = &s->tcp[i];
        if (!t->used || !t->tmask) continue;
        if (t->tmask & (1u << MT_TT_TW)) best = mt_dl_min(best, rel(t->tmr[MT_TT_TW], now));
        if (t->tmask & (1u << MT_TT_DELACK)) best = mt_dl_min(best, rel(t->tmr[MT_TT_DELACK], now));
        if (t->tmask & (1u << MT_TT_PERSIST)) best = mt_dl_min(best, rel(t->tmr[MT_TT_PERSIST], now));
        if (t->tmask & (1u << MT_TT_RTO)) {
            best = mt_dl_min(best, rel(t->tmr[MT_TT_RTO], now));
            if (ulp_ms(t)) best = mt_dl_min(best, rel(t->ulp_start + ulp_ms(t), now));
        }
    }
    return best;
}

/* ---- ARP hook ---- */

void mt_tcp_note_released(mt_stack *s, const uint8_t *ip) {
    uint16_t ihl;
    s->arp_rel.live = 0;
    if ((ip[0] >> 4) != 4 || ip[9] != 6) return;
    ihl = (uint16_t)((ip[0] & 15u) << 2);
    if (mt_rd16(ip + 2) < ihl + TCP_HDR) return;
    {
        const uint8_t *th = ip + ihl;
        uint16_t seglen = (uint16_t)(mt_rd16(ip + 2) - ihl - ((th[12] >> 4) << 2));
        if (seglen == 0 && !(th[13] & (MT_TH_SYN | MT_TH_FIN))) return;   /* pure ACK: nothing owed */
        s->arp_rel.dst = mt_rd32(ip + 16);
        s->arp_rel.sport = mt_rd16(th);
        s->arp_rel.dport = mt_rd16(th + 2);
        s->arp_rel.seq = mt_rd32(th + 4);
        s->arp_rel.live = 1;
    }
}

void mt_tcp_arp_resolved(mt_stack *s, mt_ip4 ip, uint32_t now) {
    uint16_t i;
    for (i = 0; i < s->lim.max_tcp; i++) {
        mt_tcp *t = &s->tcp[i];
        if (!t->used || t->state == MT_TCP_CLOSED || t->state == MT_TCP_LISTEN ||
            t->state == MT_TCP_TIME_WAIT)
            continue;
        if (t->snd_una == t->snd_nxt || nexthop(s, t->rip) != ip) continue;
        /* Skip a connection whose first unacked segment is the very frame that was just
         * released from the held slot: it is on the wire already. */
        if (s->arp_rel.live && s->arp_rel.dst == t->rip && s->arp_rel.sport == t->lport &&
            s->arp_rel.dport == t->rport && s->arp_rel.seq == t->snd_una)
            continue;
        mt_tcp_rexmit(t, now);
    }
    s->arp_rel.live = 0;
}

/* ---- Segment arrival ---- */

/* Options: EOL ends, NOP skips, anything else needs 2 <= len and must fit before the header
 * end. Returns -1 for a malformed list (drop the segment); *mss = 0 when absent. */
static int parse_opts(const uint8_t *o, uint16_t olen, uint16_t *mss) {
    uint16_t i = 0;
    *mss = 0;
    while (i < olen) {
        uint8_t k = o[i], l;
        if (k == 0) break;
        if (k == 1) { i++; continue; }
        if (i + 1u >= olen) return -1;
        l = o[i + 1];
        if (l < 2 || i + (uint16_t)l > olen) return -1;
        if (k == 2 && l == 4) *mss = mt_rd16(o + i + 2);
        i = (uint16_t)(i + l);
    }
    return 0;
}

static mt_tcp *lookup(mt_stack *s, mt_ip4 rip, uint16_t rport, uint16_t lport) {
    mt_tcp *best = NULL;
    int bestscore = -1;
    uint16_t i;
    for (i = 0; i < s->lim.max_tcp; i++) {
        mt_tcp *t = &s->tcp[i];
        if (!t->used || t->state == MT_TCP_CLOSED) continue;
        if (t->state == MT_TCP_LISTEN) {
            int score;
            if ((t->flport && t->flport != lport) || (t->frip && t->frip != rip) ||
                (t->frport && t->frport != rport))
                continue;
            score = (t->flport != 0) + (t->frip != 0) + (t->frport != 0);
            if (score > bestscore) { best = t; bestscore = score; }
        } else if (t->lport == lport && t->rport == rport && t->rip == rip) {
            return t;
        }
    }
    return best;
}

static void in_listen(mt_tcp *t, mt_ip4 src, uint16_t sport, uint16_t dport, const seg *g,
                      uint32_t now) {
    mt_stack *s = t->st;
    if (g->fl & MT_TH_RST) return;
    if (g->fl & MT_TH_ACK) {
        mt_tcp_send_raw(s, src, dport, sport, g->ack, 0, MT_TH_RST, now);
        return;
    }
    if (!(g->fl & MT_TH_SYN)) return;
    t->rip = src;
    t->rport = sport;
    t->lport = dport;
    t->irs = g->seq;
    t->rcv_nxt = g->seq + 1u;
    set_mss(t, g->mss);
    t->iss = isn(s, now);
    t->snd_una = t->snd_nxt = t->iss;
    t->snd_wnd = g->win;
    t->snd_wl1 = g->seq;
    t->snd_wl2 = t->iss;
    t->flags |= MT_TF_PASSIVE;
    t->state = MT_TCP_SYN_RCVD;
    s->stats.tcp_passive_opens++;
    mt_tcp_output(t, now);                 /* SYN-ACK */
}

static void established(mt_tcp *t) {
    t->state = MT_TCP_ESTABLISHED;
    t->flags |= MT_TF_EST;
    t->st->stats.tcp_conns_open++;
    t->flags &= (uint8_t)~MT_TF_PASSIVE;
}

static void in_syn_sent(mt_tcp *t, const seg *g, uint32_t now) {
    int ackok = 0;
    if (g->fl & MT_TH_ACK) {
        if (mt_seq_le(g->ack, t->iss) || mt_seq_gt(g->ack, t->snd_nxt)) {
            if (!(g->fl & MT_TH_RST))
                mt_tcp_send_raw(t->st, t->rip, t->lport, t->rport, g->ack, 0, MT_TH_RST, now);
            return;
        }
        ackok = 1;
    }
    if (g->fl & MT_TH_RST) {
        if (ackok) {
            t->st->stats.tcp_resets_in++;
            tcp_term(t, MT_TERM_OPEN_FAILED);
        }
        return;
    }
    if (!(g->fl & MT_TH_SYN)) return;
    t->irs = g->seq;
    t->rcv_nxt = g->seq + 1u;
    set_mss(t, g->mss);
    t->snd_wnd = g->win;
    t->snd_wl1 = g->seq;
    t->snd_wl2 = g->ack;
    if (ackok) {                                           /* SYN-ACK */
        t->snd_una = g->ack;
        tmr_clr(t, MT_TT_RTO);
        t->rto = MT_TCP_RTO_INIT;
        established(t);
        t->flags |= MT_TF_ACKNOW;
        mt_tcp_output(t, now);                             /* ACK (plus queued data) */
        fire(t, MT_TCP_EV_ESTABLISHED, 0);
    } else {                                               /* simultaneous open */
        t->state = MT_TCP_SYN_RCVD;
        mt_tcp_rexmit(t, now);                             /* SYN-ACK */
        t->st->stats.tcp_rexmt--;                          /* first SYN-ACK, not a retransmission */
    }
}

static void in_sync(mt_tcp *t, const seg *g, uint32_t now) {
    uint32_t free_ = mt_ring_free(&t->rcvq), seq = g->seq, dlen = g->dlen, skip, seglen;
    const uint8_t *d = g->data;
    int fin = (g->fl & MT_TH_FIN) != 0, inwin;
    unsigned ev = 0;
    uint32_t dacc = 0;
    uint8_t g0 = t->gen;

    inwin = free_ ? (seq - t->rcv_nxt) < free_ : seq == t->rcv_nxt;
    if (g->fl & MT_TH_RST) {
        if (inwin) {
            if (t->state == MT_TCP_TIME_WAIT) return;      /* never let an RST cut TIME_WAIT short */
            t->st->stats.tcp_resets_in++;
            if (t->state == MT_TCP_SYN_RCVD && (t->flags & MT_TF_PASSIVE)) back_to_listen(t);
            else tcp_term(t, MT_TERM_REMOTE_ABORT);
        } else {
            t->flags |= MT_TF_ACKNOW;                      /* challenge ACK */
            mt_tcp_output(t, now);
        }
        return;
    }
    if (g->fl & MT_TH_SYN) {
        if (t->state == MT_TCP_SYN_RCVD && seq == t->irs &&
            (g->fl & MT_TH_ACK) && mt_seq_lt(t->snd_una, g->ack) &&
            mt_seq_le(g->ack, t->snd_nxt)) {              /* simultaneous open completes */
            t->snd_una = g->ack;
            t->snd_wnd = g->win;
            t->snd_wl1 = seq;
            t->snd_wl2 = g->ack;
            tmr_clr(t, MT_TT_RTO);
            t->rto = MT_TCP_RTO_INIT;
            established(t);
            t->flags |= MT_TF_ACKNOW;
            mt_tcp_output(t, now);
            fire(t, MT_TCP_EV_ESTABLISHED, 0);
        } else if (t->state == MT_TCP_SYN_RCVD && seq == t->irs && !(g->fl & MT_TH_ACK)) {
            mt_tcp_rexmit(t, now);                         /* bare SYN: our SYN-ACK was lost */
        } else {
            t->flags |= MT_TF_ACKNOW;                      /* challenge ACK, connection kept */
            mt_tcp_output(t, now);
        }
        return;
    }
    seglen = dlen + (uint32_t)fin;
    if (seglen == 0 ? !inwin
                    : !(free_ && (inwin || (seq + seglen - 1u - t->rcv_nxt) < free_))) {
        if (free_ == 0 && seglen && seq == t->rcv_nxt && (g->fl & MT_TH_ACK) &&
            t->state != MT_TCP_TIME_WAIT) {
            dlen = 0;                    /* zero window: refuse the data but honour its ACK */
            fin = 0;
            t->flags |= MT_TF_ACKNOW;
        } else {
            if (t->state == MT_TCP_TIME_WAIT && fin) tmr_set(t, MT_TT_TW, now + TCP_TW_MS);
            t->flags |= MT_TF_ACKNOW;
            mt_tcp_output(t, now);
            return;
        }
    }
    if (!(g->fl & MT_TH_ACK)) return;

    if (t->state == MT_TCP_SYN_RCVD) {
        if (!(mt_seq_lt(t->snd_una, g->ack) && mt_seq_le(g->ack, t->snd_nxt))) {
            mt_tcp_send_raw(t->st, t->rip, t->lport, t->rport, g->ack, 0, MT_TH_RST, now);
            return;
        }
        t->snd_una = g->ack;
        t->snd_wnd = g->win;
        t->snd_wl1 = seq;
        t->snd_wl2 = g->ack;
        tmr_clr(t, MT_TT_RTO);
        t->rto = MT_TCP_RTO_INIT;
        established(t);
        ev |= EV_EST;
    }
    if (mt_seq_gt(g->ack, t->snd_nxt)) {                   /* acks something never sent */
        t->flags |= MT_TF_ACKNOW;
        goto out;
    }
    if (mt_seq_gt(g->ack, t->snd_una)) {                   /* new data acknowledged */
        uint32_t n = g->ack - t->snd_una;
        int fa = (t->flags & MT_TF_FINSENT) && g->ack == t->snd_nxt;
        n -= (uint32_t)fa;
        mt_ring_consume(&t->sndq, n);
        t->snd_una = g->ack;
        t->rto = MT_TCP_RTO_INIT;
        t->ulp_start = now;
        if (t->snd_una == t->snd_nxt) tmr_clr(t, MT_TT_RTO);
        else tmr_set(t, MT_TT_RTO, now + t->rto);
        if (fa) {
            if (t->state == MT_TCP_FIN_WAIT_1) {
                t->state = MT_TCP_FIN_WAIT_2;
            } else if (t->state == MT_TCP_CLOSING) {
                enter_time_wait(t, now);
            } else if (t->state == MT_TCP_LAST_ACK) {
                tcp_term(t, MT_TERM_ULP_CLOSE);
                return;
            }
        }
    }
    if (mt_seq_ge(g->ack, t->snd_una) &&                  /* RFC 793 window update */
        (mt_seq_lt(t->snd_wl1, seq) || (t->snd_wl1 == seq && mt_seq_le(t->snd_wl2, g->ack)))) {
        t->snd_wnd = g->win;
        t->snd_wl1 = seq;
        t->snd_wl2 = g->ack;
    }

    if ((dlen || fin) &&
        (t->state == MT_TCP_ESTABLISHED || t->state == MT_TCP_FIN_WAIT_1 ||
         t->state == MT_TCP_FIN_WAIT_2)) {
        if (mt_seq_lt(seq, t->rcv_nxt)) {                  /* retransmission overlapping old data */
            skip = t->rcv_nxt - seq;
            if (skip > dlen) { dlen = 0; fin = 0; }
            else { d += skip; dlen -= skip; }
            seq = t->rcv_nxt;
        }
        if (dlen > free_) { dlen = free_; fin = 0; }
        if (seq != t->rcv_nxt) {                           /* a gap: no reassembly, duplicate ACK */
            t->flags |= MT_TF_ACKNOW;
            goto out;
        }
        if (dlen) {
            mt_ring_write(&t->rcvq, d, dlen);
            t->rcv_nxt += dlen;
            t->flags |= MT_TF_ACKNOW;
            ev |= EV_DATA;
            dacc = dlen;
        }
        if (fin) {
            t->rcv_nxt++;
            t->flags |= MT_TF_ACKNOW;
            ev |= EV_CLOSING;
            if (t->state == MT_TCP_ESTABLISHED) t->state = MT_TCP_CLOSE_WAIT;
            else if (t->state == MT_TCP_FIN_WAIT_1) t->state = MT_TCP_CLOSING;
            else enter_time_wait(t, now);
        }
    }
out:
    mt_tcp_output(t, now);
    /* A callback may abort/release/re-listen: gen changes and the rest of the batch is stale. */
    if (ev & EV_EST) fire(t, MT_TCP_EV_ESTABLISHED, 0);
    if ((ev & EV_DATA) && t->gen == g0) fire(t, MT_TCP_EV_DATA, dacc);
    if ((ev & EV_CLOSING) && t->gen == g0) fire(t, MT_TCP_EV_CLOSING, 0);
}

void mt_tcp_input(mt_stack *s, mt_ip4 src, mt_ip4 dst, const uint8_t *pl, uint16_t len,
                  uint32_t now) {
    seg g;
    mt_tcp *t;
    uint16_t hl, sport, dport;
    if (len < TCP_HDR || dst != s->cfg.addr || src == 0 || src == MT_ETH_BCAST_IP) goto drop;
    hl = (uint16_t)((pl[12] >> 4) << 2);
    if (hl < TCP_HDR || hl > len) goto drop;
    if (mt_cksum_fold(mt_cksum_add(mt_tcp_pseudo(src, dst, len), pl, len)) != 0) goto drop;
    if (parse_opts(pl + TCP_HDR, (uint16_t)(hl - TCP_HDR), &g.mss) < 0) goto drop;
    s->stats.tcp_in++;
    sport = mt_rd16(pl);
    dport = mt_rd16(pl + 2);
    g.seq = mt_rd32(pl + 4);
    g.ack = mt_rd32(pl + 8);
    g.fl = (uint8_t)(pl[13] & 0x17u);                      /* FIN SYN RST ACK; PSH/URG ignored */
    g.win = mt_rd16(pl + 14);
    g.data = pl + hl;
    g.dlen = (uint16_t)(len - hl);
    t = lookup(s, src, sport, dport);
    if (!t) {                                              /* no connection: reset the sender */
        if (g.fl & MT_TH_RST) return;
        if (g.fl & MT_TH_ACK)
            mt_tcp_send_raw(s, src, dport, sport, g.ack, 0, MT_TH_RST, now);
        else
            mt_tcp_send_raw(s, src, dport, sport, 0,
                            g.seq + g.dlen + ((g.fl & MT_TH_SYN) != 0) + ((g.fl & MT_TH_FIN) != 0),
                            MT_TH_RST | MT_TH_ACK, now);
        return;
    }
    t->tnow = now;
    switch (t->state) {
    case MT_TCP_LISTEN:   in_listen(t, src, sport, dport, &g, now); break;
    case MT_TCP_SYN_SENT: in_syn_sent(t, &g, now); break;
    default:              in_sync(t, &g, now); break;
    }
    return;
drop:
    s->stats.tcp_in_drops++;
}
