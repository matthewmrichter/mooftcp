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

/* 16 TCBs must fit the 16 KB stack budget: ~180 bytes with 32-bit pointers. */
typedef char mt_tcp_size_ok[sizeof(struct mt_tcp) <= 300u ? 1 : -1];

enum { EV_EST = 1, EV_DATA = 2, EV_CLOSING = 4, EV_SPACE = 8, EV_URG = 16 };

typedef struct {
    const uint8_t *data;
    uint32_t seq, ack;
    uint32_t acc_off, acc_n;     /* data[acc_off .. +acc_n) is accepted at rcv_nxt (see plan()) */
    uint16_t dlen, win, mss, up;
    uint8_t fl;
    uint8_t acc_fin;             /* the FIN is accepted too */
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
    t->flags2 = 0;
    t->tmask = 0;
    mt_tcp_set_state(t, MT_TCP_CLOSED);
    mt_log(t->st, MT_LOG_TCP_TERM, (uint8_t)r, t->lport);
    t->gen++;
    fire(t, MT_TCP_EV_TERMINATE, r);
}

/* Reset connection variables and rings for a new open/listen. */
static void conn_reset(mt_tcp *t) {
    mt_ring_init(&t->sndq, t->sndq.base, t->sndq.size);
    mt_ring_init(&t->rcvq, t->rcvq.base, t->rcvq.size);
    t->snd_una = t->snd_nxt = t->snd_max = t->snd_wnd = t->rcv_nxt = 0;
    t->snd_wl1 = t->snd_wl2 = t->iss = t->irs = 0;
    t->tmask = 0;
    t->rto = MT_TCP_RTO_INIT;
    t->srtt8 = t->rttvar4 = 0;
    t->dupacks = 0;
    t->flags &= MT_TF_ULPABORT;
    t->flags2 = 0;
    t->ooo_hi = 0;
    t->rcv_adv = t->rcv_up = t->snd_up = 0;
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
    t->ulp_ms = TCP_ULP_DEFAULT_S * 1000u;
    t->flags = MT_TF_ULPABORT;
    t->rto = MT_TCP_RTO_INIT;
    t->mss = TCP_MSS_DEFAULT;
    t->state = MT_TCP_CLOSED;       /* creation is not logged */
    return t;
}

void mt_tcp_set_ulp_timeout(mt_tcp *t, uint16_t seconds, int abort) {
    t->ulp_ms = (uint32_t)seconds * 1000u;
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
    s->now = now;
    conn_reset(t);
    t->rip = rip;
    t->rport = rport;
    t->lport = lport;
    t->flport = t->frport = 0;
    t->frip = 0;
    t->tnow = now;
    t->iss = isn(s, now);
    t->snd_una = t->snd_nxt = t->snd_max = t->iss;   /* SYN goes out below, advancing snd_nxt */
    mt_tcp_set_state(t, MT_TCP_SYN_SENT);
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
    mt_tcp_set_state(t, MT_TCP_LISTEN);
    return MT_OK;
}

/* RST for a synchronized connection. */
static void send_rst(mt_tcp *t, uint32_t now) {
    if (is_sync(t->state))
        mt_tcp_send_raw(t->st, t->rip, t->lport, t->rport, t->snd_max, t->rcv_nxt,
                        MT_TH_RST | MT_TH_ACK, now);
}

static void back_to_listen(mt_tcp *t) {
    t->tmask = 0;
    t->flags &= MT_TF_ULPABORT;
    t->flags2 = 0;
    mt_tcp_set_state(t, MT_TCP_LISTEN);
    t->rip = 0;
    t->rport = 0;
    t->lport = t->flport;
}

int mt_tcp_close(mt_tcp *t, uint32_t now) {
    t->tnow = now;
    t->st->now = now;
    switch (t->state) {
    case MT_TCP_LISTEN:
        mt_tcp_set_state(t, MT_TCP_CLOSED);
        return MT_OK;
    case MT_TCP_SYN_SENT:
        tcp_term(t, MT_TERM_ULP_CLOSE);
        return MT_OK;
    case MT_TCP_SYN_RCVD:
        send_rst(t, now);
        tcp_term(t, MT_TERM_ULP_CLOSE);
        return MT_OK;
    case MT_TCP_ESTABLISHED:
        mt_tcp_set_state(t, MT_TCP_FIN_WAIT_1);
        break;
    case MT_TCP_CLOSE_WAIT:
        mt_tcp_set_state(t, MT_TCP_LAST_ACK);
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
    t->st->now = now;
    if (t->state == MT_TCP_LISTEN) { mt_tcp_set_state(t, MT_TCP_CLOSED); return; }
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
        sent = t->snd_max - t->snd_una - ((t->flags & MT_TF_FINSENT) ? 1u : 0u);
        i->unacked = sent;
        i->send_queued = t->sndq.used > sent ? t->sndq.used - sent : 0u;
    }
    i->rcv_avail = t->rcvq.used;
    i->snd_wnd = t->snd_wnd;
    i->rcv_wnd = mt_ring_free(&t->rcvq);
    i->srtt_ms = t->srtt8 >> 3;
    i->rto_ms = t->rto;
}

/* ---- Data transfer API ---- */

int32_t mt_tcp_send(mt_tcp *t, const uint8_t *p, uint32_t len, int push, int urgent,
                    uint32_t now) {
    uint32_t n;
    if (t->state != MT_TCP_ESTABLISHED && t->state != MT_TCP_CLOSE_WAIT) return MT_ERR_STATE;
    t->tnow = now;
    n = mt_ring_write(&t->sndq, p, len);
    if (n < len) t->flags2 |= MT_T2_SNDWANT;               /* SEND_SPACE owed once ACKs free room */
    if (push) t->flags2 |= MT_T2_PUSHQ;                    /* disables the Nagle hold */
    if (urgent && n) {
        t->snd_up = t->snd_una + t->sndq.used;             /* just past the last queued byte */
        t->flags2 |= MT_T2_SNDURG;
    }
    mt_tcp_output(t, now);                                 /* leaves before we return if allowed */
    return (int32_t)n;
}

/* Receive buffer space was freed by the app: tell the peer if the window reopens meaningfully
 * (from below one MSS to at least one, or by at least half the ring). */
static void rcv_freed(mt_tcp *t, uint32_t now) {
    uint32_t cur, win;
    if ((t->flags2 & MT_T2_RCVURG) && !mt_seq_gt(t->rcv_up, t->rcv_nxt - t->rcvq.used))
        t->flags2 &= (uint8_t)~MT_T2_RCVURG;               /* mark passed: keeps the compare sane */
    if (t->state != MT_TCP_ESTABLISHED && t->state != MT_TCP_FIN_WAIT_1 &&
        t->state != MT_TCP_FIN_WAIT_2)
        return;
    cur = t->rcv_adv - t->rcv_nxt;
    if (cur & 0x80000000u) cur = 0;
    win = mt_tcp_rcv_window(t, 0);                         /* receiver SWS already applied */
    if (win > cur && ((cur < t->mss && win >= t->mss) || win - cur >= t->rcvq.size / 2u)) {
        t->flags |= MT_TF_ACKNOW;
        mt_tcp_output(t, now);
    }
}

void mt_tcp_consume(mt_tcp *t, uint32_t n, uint32_t now) {
    t->tnow = now;
    mt_ring_consume(&t->rcvq, n);
    rcv_freed(t, now);
}

uint32_t mt_tcp_recv(mt_tcp *t, uint8_t *dst, uint32_t max, uint32_t now) {
    uint32_t n = mt_ring_read(&t->rcvq, dst, max);
    t->tnow = now;
    if (n) rcv_freed(t, now);
    return n;
}

uint32_t mt_tcp_peek(mt_tcp *t, uint32_t off, const uint8_t **p) {
    return mt_ring_peek(&t->rcvq, off, p);
}

uint32_t mt_tcp_send_free(const mt_tcp *t) { return mt_ring_free(&t->sndq); }

int mt_tcp_urgent(const mt_tcp *t, uint32_t *bytes_to_mark) {
    uint32_t rd = t->rcv_nxt - t->rcvq.used;
    if (!(t->flags2 & MT_T2_RCVURG) || !mt_seq_gt(t->rcv_up, rd)) return 0;
    if (bytes_to_mark) *bytes_to_mark = t->rcv_up - rd;
    return 1;
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
    mt_tcp_set_state(t, MT_TCP_TIME_WAIT);
    tmr_clr(t, MT_TT_RTO);
    tmr_clr(t, MT_TT_DELACK);
    tmr_set(t, MT_TT_TW, now + TCP_TW_MS);
}

#define TM_RTO_PERSIST ((1u << MT_TT_RTO) | (1u << MT_TT_PERSIST))
#define CWND_MAX 0x100000u

static uint32_t backoff(uint32_t rto) { return rto >= MT_TCP_RTO_MAX / 2u ? MT_TCP_RTO_MAX : rto << 1; }

/* ULP timeout: no progress on unacked data (or no answer to window probes) for ulp_ms. The
 * deadline is the absolute ulp_dl, set when a timer arms and pushed out by progress. */
static void ulp_expired(mt_tcp *t, uint32_t now) {
    uint8_t g0 = t->gen;
    if (!(t->flags & MT_TF_ULPABORT)) {
        t->ulp_dl = now + t->ulp_ms;                       /* report only: keep retrying */
        fire(t, MT_TCP_EV_ULP_TIMEOUT, 0);
    } else if (t->state == MT_TCP_SYN_SENT) {
        tcp_term(t, MT_TERM_OPEN_FAILED);
    } else if (t->state == MT_TCP_SYN_RCVD && (t->flags & MT_TF_PASSIVE)) {
        back_to_listen(t);
    } else {
        fire(t, MT_TCP_EV_ULP_TIMEOUT, 0);
        if (t->gen == g0) {
            send_rst(t, now);
            tcp_term(t, MT_TERM_ULP_TIMEOUT);
        }
    }
}

/* Retransmission timeout. Data states: back to Loss Window (cwnd = 1 MSS, ssthresh halved) and
 * go-back-N from snd_una; SYN states resend the SYN / SYN-ACK. */
static void rto_fire(mt_tcp *t, uint32_t now) {
    if (t->state == MT_TCP_SYN_SENT || t->state == MT_TCP_SYN_RCVD) {
        mt_tcp_rexmit(t, now);
    } else {
        uint32_t half = (t->snd_max - t->snd_una) >> 1, floor_ = (uint32_t)t->mss << 1;
        t->ssthresh = half > floor_ ? half : floor_;
        t->cwnd = t->mss;
        t->ca_acc = 0;
        t->dupacks = 0;
        t->recover = t->snd_max;
        t->flags2 &= (uint8_t)~(MT_T2_RECOV | MT_T2_RTTING);
        t->snd_nxt = t->snd_una;
        mt_tcp_output(t, now);
    }
    t->rto = backoff(t->rto);
    if (t->tmask & (1u << MT_TT_RTO)) tmr_set(t, MT_TT_RTO, now + t->rto);
}

void mt_tcp_timers(mt_stack *s, uint32_t now) {
    uint16_t i;
    for (i = 0; i < s->lim.max_tcp; i++) {
        mt_tcp *t = &s->tcp[i];
        if (!t->used || !t->tmask) continue;
        t->tnow = now;
        if ((t->tmask & (1u << MT_TT_TW)) && !mt_time_before(now, t->tmr[MT_TT_TW])) {
            if (t->state == MT_TCP_FIN_WAIT_2) {
                /* Backstop against TCB exhaustion (MacTCP has no FIN_WAIT_2 timeout): behaves
                 * like the ULP timeout. */
                uint8_t g0 = t->gen;
                tmr_set(t, MT_TT_TW, now + MT_TCP_FW2_MS);
                fire(t, MT_TCP_EV_ULP_TIMEOUT, 0);
                if (t->gen == g0 && (t->flags & MT_TF_ULPABORT)) {
                    send_rst(t, now);
                    tcp_term(t, MT_TERM_ULP_TIMEOUT);
                }
                continue;
            }
            tcp_term(t, MT_TERM_ULP_CLOSE);                 /* TIME_WAIT over */
            continue;
        }
        if ((t->tmask & (1u << MT_TT_DELACK)) && !mt_time_before(now, t->tmr[MT_TT_DELACK])) {
            tmr_clr(t, MT_TT_DELACK);
            t->flags |= MT_TF_ACKNOW;
            mt_tcp_output(t, now);
        }
        if (!(t->tmask & TM_RTO_PERSIST)) continue;
        if (t->ulp_ms && !mt_time_before(now, t->ulp_dl)) {
            ulp_expired(t, now);
        } else if ((t->tmask & (1u << MT_TT_RTO)) && !mt_time_before(now, t->tmr[MT_TT_RTO])) {
            rto_fire(t, now);
        } else if ((t->tmask & (1u << MT_TT_PERSIST)) &&
                   !mt_time_before(now, t->tmr[MT_TT_PERSIST])) {
            mt_tcp_probe(t, now);
            t->rto = backoff(t->rto);
            tmr_set(t, MT_TT_PERSIST, now + t->rto);
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
        if (t->tmask & (1u << MT_TT_RTO)) best = mt_dl_min(best, rel(t->tmr[MT_TT_RTO], now));
        if ((t->tmask & TM_RTO_PERSIST) && t->ulp_ms) best = mt_dl_min(best, rel(t->ulp_dl, now));
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
        uint16_t space = (uint16_t)(seglen + ((th[13] & MT_TH_SYN) != 0) + ((th[13] & MT_TH_FIN) != 0));
        if (space == 0) return;                                   /* pure ACK: nothing owed */
        s->arp_rel.len = space;
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
        uint32_t skip = 0;
        if (!t->used || t->state == MT_TCP_CLOSED || t->state == MT_TCP_LISTEN ||
            t->state == MT_TCP_TIME_WAIT)
            continue;
        if (t->snd_una == t->snd_max || nexthop(s, t->rip) != ip) continue;
        /* The very frame just released from the held slot is first-unacked and already on the
         * wire: resend only what follows it. */
        if (s->arp_rel.live && s->arp_rel.dst == t->rip && s->arp_rel.sport == t->lport &&
            s->arp_rel.dport == t->rport && s->arp_rel.seq == t->snd_una)
            skip = s->arp_rel.len;
        if (skip >= t->snd_max - t->snd_una) continue;
        if (t->state == MT_TCP_SYN_SENT || t->state == MT_TCP_SYN_RCVD) {
            mt_tcp_rexmit(t, now);
            continue;
        }
        /* Go-back-N: frames sent while the next hop was unresolved were dropped (only one small
         * frame is held), so resend from the ring, bounded by cwnd. Not a congestion event. */
        t->flags2 &= (uint8_t)~MT_T2_RTTING;
        t->snd_nxt = t->snd_una + skip;
        mt_tcp_output(t, now);
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
    t->rcv_adv = t->rcv_nxt;
    set_mss(t, g->mss);
    t->iss = isn(s, now);
    t->snd_una = t->snd_nxt = t->snd_max = t->iss;
    t->snd_wnd = g->win;
    t->snd_wl1 = g->seq;
    t->snd_wl2 = t->iss;
    t->flags |= MT_TF_PASSIVE;
    mt_tcp_set_state(t, MT_TCP_SYN_RCVD);
    s->stats.tcp_passive_opens++;
    mt_tcp_output(t, now);                 /* SYN-ACK */
}

/* RFC 6298 estimator in shifts: srtt8 = 8*SRTT, rttvar4 = 4*RTTVAR (ms). */
static void rtt_sample(mt_tcp *t, uint32_t r) {
    if (r == 0) r = 1;                                     /* srtt8 == 0 means unmeasured */
    if (r > 0xFFFFFu) r = 0xFFFFFu;
    if (t->srtt8 == 0) {
        t->srtt8 = r << 3;
        t->rttvar4 = r << 1;
    } else {
        int32_t err = (int32_t)r - (int32_t)(t->srtt8 >> 3);
        t->srtt8 += (uint32_t)err;
        if (err < 0) err = -err;
        err -= (int32_t)(t->rttvar4 >> 2);
        t->rttvar4 += (uint32_t)err;
    }
}

/* New data was acked (snd_una already advanced): RTT sample, congestion window, RTO and timer
 * rearm in one pass; no loops, no divisions. */
static void ack_progress(mt_tcp *t, uint32_t acked, uint32_t now) {
    if ((t->flags2 & MT_T2_RTTING) && mt_seq_ge(t->snd_una, t->rtt_seq)) {
        t->flags2 &= (uint8_t)~MT_T2_RTTING;
        rtt_sample(t, now - t->rtt_t0);
    }
    t->dupacks = 0;
    if (t->flags2 & MT_T2_RECOV) {
        if (mt_seq_ge(t->snd_una, t->recover)) {
            t->flags2 &= (uint8_t)~MT_T2_RECOV;            /* full ACK: recovery over */
            t->cwnd = t->ssthresh;
        } else {
            mt_tcp_rexmit(t, now);                         /* partial ACK: the next hole */
        }
    } else if (t->cwnd < t->ssthresh) {
        t->cwnd += acked < t->mss ? acked : t->mss;        /* slow start */
    } else {
        t->ca_acc += acked;                                /* congestion avoidance: +1 MSS per cwnd acked */
        if (t->ca_acc >= t->cwnd) {
            t->ca_acc -= t->cwnd;
            t->cwnd += t->mss;
        }
    }
    if (t->cwnd > CWND_MAX) t->cwnd = CWND_MAX;
    t->rto = mt_tcp_rto_base(t);
    t->ulp_dl = now + t->ulp_ms;
    if (t->snd_una == t->snd_max) tmr_clr(t, MT_TT_RTO);
    else if (!(t->tmask & (1u << MT_TT_PERSIST))) tmr_set(t, MT_TT_RTO, now + t->rto);
}

static void established(mt_tcp *t) {
    uint32_t m4 = (uint32_t)t->mss << 2, m2 = (uint32_t)t->mss << 1;
    t->cwnd = m4 < 4380u ? m4 : (m2 > 4380u ? m2 : 4380u);   /* min(4*MSS, max(2*MSS, 4380)) */
    t->ssthresh = 0xFFFFFFFFu;
    t->ca_acc = 0;
    t->dupacks = 0;
    t->recover = t->snd_una - 1u;
    mt_tcp_set_state(t, MT_TCP_ESTABLISHED);
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
    t->rcv_adv = t->rcv_nxt;
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
        mt_tcp_set_state(t, MT_TCP_SYN_RCVD);
        mt_tcp_xmit(t, t->iss, MT_TH_SYN | MT_TH_ACK, 0, 0, now);   /* first SYN-ACK */
    }
}

/* Which part of the segment's data is acceptable now: the bytes at rcv_nxt (an overlapping
 * retransmission is trimmed at the front, anything past the free ring space at the back), plus
 * the FIN if all the data fits. Pure: mt_tcp_input calls it before the checksum is known so the
 * payload can be copied into the ring in the same pass as it is summed; in_sync commits it only
 * after the checksum verified. Out of order, duplicate, zero-window and wrong-state segments get
 * acc_n == 0 and are checksummed only. */
static void plan(const mt_tcp *t, seg *g) {
    uint32_t free_, dlen = g->dlen, skip = 0;
    int fin = (g->fl & MT_TH_FIN) != 0;
    g->acc_off = g->acc_n = 0;
    g->acc_fin = 0;
    if (t->state != MT_TCP_ESTABLISHED && t->state != MT_TCP_FIN_WAIT_1 &&
        t->state != MT_TCP_FIN_WAIT_2 && t->state != MT_TCP_SYN_RCVD)
        return;
    free_ = mt_ring_free(&t->rcvq);
    if (free_ == 0 || (g->fl & (MT_TH_SYN | MT_TH_RST))) return;
    if (mt_seq_lt(g->seq, t->rcv_nxt)) {                   /* retransmission overlapping old data */
        skip = t->rcv_nxt - g->seq;
        if (skip > dlen) return;
        dlen -= skip;
    } else if (g->seq != t->rcv_nxt) {
        return;                                            /* a gap: no reassembly in phase 1 */
    }
    if (dlen > free_) { dlen = free_; fin = 0; }
    g->acc_off = skip;
    g->acc_n = dlen;
    g->acc_fin = (uint8_t)fin;
}

static void in_sync(mt_tcp *t, const seg *g, uint32_t now) {
    uint32_t free_ = mt_ring_free(&t->rcvq), seq = g->seq, dlen = g->dlen, seglen;
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
            else if ((t->state == MT_TCP_LAST_ACK || t->state == MT_TCP_CLOSING) &&
                     (t->flags & MT_TF_FINSENT) && t->snd_una == t->snd_max - 1u)
                tcp_term(t, MT_TERM_ULP_CLOSE);            /* all data acked, only our FIN open:
                                                            * the peer already finished (its
                                                            * TIME_WAIT expired): a clean close */
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
    if (t->state == MT_TCP_FIN_WAIT_2 && (t->tmask & (1u << MT_TT_TW)))
        tmr_set(t, MT_TT_TW, now + MT_TCP_FW2_MS);         /* any acceptable segment: peer is alive */
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
    if (mt_seq_gt(g->ack, t->snd_max)) {                   /* acks something never sent */
        t->flags |= MT_TF_ACKNOW;
        goto out;
    }
    if (t->tmask & (1u << MT_TT_PERSIST))                  /* a peer that answers is not gone: it has */
        t->ulp_dl = t->tmr[MT_TT_PERSIST] + t->ulp_ms;     /* until a full ULP after the next probe */
    if (g->ack == t->snd_una) {
        /* Duplicate ACK (RFC 5681): no data, window unchanged, data outstanding, not a probe
         * answer. The third one fast-retransmits the first unacked segment; recovery then
         * (NewReno-lite) ends at an ACK >= recover, resending the next hole on partial ACKs. */
        if (t->snd_una != t->snd_max && g->dlen == 0 && !fin && g->win == t->snd_wnd &&
            !(t->tmask & (1u << MT_TT_PERSIST))) {
            if (t->dupacks < 255u) t->dupacks++;
            if (t->dupacks == 3 && mt_seq_gt(t->snd_una, t->recover)) {
                uint32_t half = (t->snd_max - t->snd_una) >> 1, floor_ = (uint32_t)t->mss << 1;
                t->ssthresh = half > floor_ ? half : floor_;
                t->cwnd = t->ssthresh;
                t->recover = t->snd_max;
                t->flags2 |= MT_T2_RECOV;
                t->st->stats.tcp_fast_rexmt++;
                mt_tcp_rexmit(t, now);
            }
        }
    } else if (mt_seq_gt(g->ack, t->snd_una)) {            /* new data acknowledged (older ACKs: ignored) */
        uint32_t acked = g->ack - t->snd_una, n = acked;
        int fa = (t->flags & MT_TF_FINSENT) && g->ack == t->snd_max;
        n -= (uint32_t)fa;
        mt_ring_consume(&t->sndq, n);
        if (n && (t->flags2 & MT_T2_SNDWANT)) {            /* a short send is waiting for room */
            t->flags2 &= (uint8_t)~MT_T2_SNDWANT;
            ev |= EV_SPACE;
        }
        t->snd_una = g->ack;
        if (mt_seq_lt(t->snd_nxt, g->ack)) t->snd_nxt = g->ack;   /* go-back-N: peer had more */
        if ((t->flags2 & MT_T2_SNDURG) && mt_seq_ge(g->ack, t->snd_up))
            t->flags2 &= (uint8_t)~MT_T2_SNDURG;
        ack_progress(t, acked, now);
        if (fa) {
            if (t->state == MT_TCP_FIN_WAIT_1) {
                mt_tcp_set_state(t, MT_TCP_FIN_WAIT_2);
                tmr_set(t, MT_TT_TW, now + MT_TCP_FW2_MS);
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

    if (t->state == MT_TCP_ESTABLISHED || t->state == MT_TCP_FIN_WAIT_1 ||
        t->state == MT_TCP_FIN_WAIT_2) {
        if ((g->fl & MT_TH_URG) && !mt_seq_gt(seq, t->rcv_nxt)) {   /* urgent pointer advances? */
            uint32_t ue = seq + g->up, rd = t->rcv_nxt - t->rcvq.used;
            if (mt_seq_gt(ue, rd) &&
                (!(t->flags2 & MT_T2_RCVURG) || mt_seq_gt(ue, t->rcv_up))) {
                t->rcv_up = ue;
                t->flags2 |= MT_T2_RCVURG;
                ev |= EV_URG;
            }
        }
        if (g->acc_n || g->acc_fin) {
            if (g->acc_n) {                                /* commit what mt_tcp_input copied */
                t->rcvq.used += g->acc_n;
                t->rcv_nxt += g->acc_n;
                ev |= EV_DATA;
                dacc = g->acc_n;
                /* ACK policy: sub-MSS, PSH or FIN -> ACK before mt_input returns (output below,
                 * after the callbacks, so a reply sent by the app carries it); a full segment is
                 * ACKed with every second one, else by the 100 ms timer; while MT_T2_GAP is set (a hole is
                 * being repaired) every in-order segment is ACKed at once. */
                if (g->acc_n >= t->mss && !(g->fl & MT_TH_PSH) && !g->acc_fin &&
                    !(t->flags2 & (MT_T2_ACKHALF | MT_T2_GAP))) {
                    t->flags2 |= MT_T2_ACKHALF;
                    if (!(t->tmask & (1u << MT_TT_DELACK)))
                        tmr_set(t, MT_TT_DELACK, now + MT_TCP_DELACK_MS);
                } else {
                    t->flags |= MT_TF_ACKNOW;
                }
            }
            if (g->acc_fin) {
                t->rcv_nxt++;
                t->flags |= MT_TF_ACKNOW;
                ev |= EV_CLOSING;
                if (t->state == MT_TCP_ESTABLISHED) mt_tcp_set_state(t, MT_TCP_CLOSE_WAIT);
                else if (t->state == MT_TCP_FIN_WAIT_1) mt_tcp_set_state(t, MT_TCP_CLOSING);
                else enter_time_wait(t, now);
            }
            if ((t->flags2 & MT_T2_GAP) && mt_seq_ge(t->rcv_nxt, t->ooo_hi))
                t->flags2 &= (uint8_t)~MT_T2_GAP;          /* the hole is filled */
        } else if (dlen || fin) {                          /* a gap or duplicate: duplicate ACK */
            uint32_t end = seq + dlen + (uint32_t)fin;
            t->flags |= MT_TF_ACKNOW;
            if (mt_seq_gt(seq, t->rcv_nxt)) {              /* RFC 5681 4.2: while a hole is open, */
                if (!(t->flags2 & MT_T2_GAP) || mt_seq_gt(end, t->ooo_hi))   /* ACK every segment */
                    t->ooo_hi = end;                       /* that advances rcv_nxt (no delayed ACK) */
                t->flags2 |= MT_T2_GAP;
            }
        }
    }
out:
    /* Data events run before the ACK goes out so a reply from the callback carries it. Other
     * work (ACK, window, queued data) is sent first. A callback may abort/release/re-listen:
     * gen changes and the rest of the batch is stale. */
    if (!(ev & EV_DATA)) mt_tcp_output(t, now);
    if (ev & EV_EST) fire(t, MT_TCP_EV_ESTABLISHED, 0);
    if ((ev & EV_URG) && t->gen == g0)
        fire(t, MT_TCP_EV_URGENT, t->rcv_up - (t->rcv_nxt - t->rcvq.used));
    if ((ev & EV_DATA) && t->gen == g0) fire(t, MT_TCP_EV_DATA, dacc);
    if ((ev & EV_CLOSING) && t->gen == g0) fire(t, MT_TCP_EV_CLOSING, 0);
    if ((ev & EV_SPACE) && t->gen == g0) fire(t, MT_TCP_EV_SEND_SPACE, mt_ring_free(&t->sndq));
    if ((ev & EV_DATA) && t->gen == g0) mt_tcp_output(t, now);
}

/* Sum the segment's payload for the checksum. Accepted bytes are copied into the free space of
 * the receive ring in the same pass (nothing is committed until the sum verifies); the skipped
 * front and the trimmed tail are summed only. Chunk sums are computed as if each started at an
 * even offset and re-based with mt_cksum_odd where the chunk really starts at an odd stream
 * offset. */
static uint32_t data_sum(mt_tcp *t, const seg *g) {
    uint32_t n = g->acc_n, skip = g->acc_off, size, pos, c1, part, sum, tail;
    const uint8_t *d;
    uint8_t *base;
    if (n == 0) return mt_cksum_add(0, g->data, g->dlen);
    size = t->rcvq.size;
    base = t->rcvq.base;
    pos = t->rcvq.head + t->rcvq.used;
    if (pos >= size) pos -= size;
    d = g->data + skip;
    c1 = size - pos;
    if (c1 > n) c1 = n;
    part = mt_cksum_copy(0, base + pos, d, c1);
    if (n > c1) {                                          /* the free space wraps: two chunks */
        uint32_t p2 = mt_cksum_copy(0, base, d + c1, n - c1);
        part += (c1 & 1u) ? mt_cksum_odd(p2) : p2;
    }
    if (skip & 1u) part = mt_cksum_odd(part);
    sum = (skip ? mt_cksum_add(0, g->data, skip) : 0u) + part;
    tail = g->dlen - skip - n;
    if (tail) {
        uint32_t ts = mt_cksum_add(0, d + n, tail);
        sum += ((skip + n) & 1u) ? mt_cksum_odd(ts) : ts;
    }
    return sum;
}

void mt_tcp_input(mt_stack *s, mt_ip4 src, mt_ip4 dst, const uint8_t *pl, uint16_t len,
                  uint32_t now) {
    seg g;
    mt_tcp *t;
    uint32_t sum;
    uint16_t hl, sport, dport, why;
    if (len < TCP_HDR || dst != s->cfg.addr || src == 0 || src == MT_ETH_BCAST_IP) MT_DROP_GOTO(why);
    hl = (uint16_t)((pl[12] >> 4) << 2);
    if (hl < TCP_HDR || hl > len) MT_DROP_GOTO(why);
    if (parse_opts(pl + TCP_HDR, (uint16_t)(hl - TCP_HDR), &g.mss) < 0) MT_DROP_GOTO(why);
    sport = mt_rd16(pl);
    dport = mt_rd16(pl + 2);
    g.seq = mt_rd32(pl + 4);
    g.ack = mt_rd32(pl + 8);
    g.fl = (uint8_t)(pl[13] & 0x3Fu);                      /* FIN SYN RST PSH ACK URG */
    g.win = mt_rd16(pl + 14);
    g.up = mt_rd16(pl + 18);
    g.data = pl + hl;
    g.dlen = (uint16_t)(len - hl);
    /* Single pass over the payload: the header sum first (hl is a multiple of 4, so chaining
     * is exact), then payload bytes are copied into the receive ring while summed. The
     * connection is found, and the acceptable part of the data planned, before the checksum
     * is known; only the ring's unused space is touched until it verifies. */
    t = lookup(s, src, sport, dport);
    g.acc_off = g.acc_n = 0;
    g.acc_fin = 0;
    if (t) plan(t, &g);
    sum = mt_cksum_add(mt_tcp_pseudo(src, dst, len), pl, hl);
    sum += data_sum(t, &g);
    if (mt_cksum_fold(sum) != 0) MT_DROP_GOTO(why);
    s->stats.tcp_in++;
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
    MT_LOG_DROPPED(s, MT_LOGL_TCP, why);
}
