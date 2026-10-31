#include "t.h"
#include "net.h"
#include "mt_int.h"
#include "mt_util.h"
#include "pbfake.h"

/* Stateful system fuzzer. One stack, one simulated peer that crafts frames from the stack's live
 * state (TCB sequence numbers, pending DNS ids, ...) and from raw mutations of those frames,
 * odd frame addresses, reflections of the stack's own output, and callbacks that re-enter the API
 * (send, recv, close, abort, release, re-listen, DNS, UDP, ICMP). Everything is driven by a
 * xorshift32 PRNG with a fixed seed, so a failure is reproduced by its "seed S step N" line.
 *
 * Properties checked on every step:
 *  - each frame handed to link.tx is valid: Ethernet source = us, IPv4 and TCP/UDP/ICMP
 *    checksums verify, source address = ours, length <= MTU, no port 0, no multicast/zero
 *    destination, ARP frames exactly 60 bytes;
 *  - every TCP payload byte we transmit equals the pattern byte for its sequence number and port
 *    (send requests are filled with that pattern), so go-back-N, probes and fast retransmits that
 *    re-read caller segments across odd segment boundaries are checked byte by byte;
 *  - send requests (1..8 segments, odd lengths and addresses, zero-length entries, up to two
 *    queued, invalid ones too) complete exactly once each, in submission order, MT_ERR_ABORTED only
 *    on a closed TCB; their segments, the canary bytes between them and the mt_seg array are never
 *    written; refusals (MT_ERR_INVAL / MT_ERR_BUSY / MT_ERR_STATE) match the request and state;
 *  - no callback fires on a freed TCB;
 *  - TCB invariants: receive ring within bounds, snd_una <= snd_nxt <= snd_max, unacked data
 *    never exceeds the queued bytes (+1 for the FIN), the send cursor points at the byte at snd_una
 *    and the bytes left per request add up, no completed request is left un-notified between calls,
 *    RTO and PERSIST never both armed, unacked data always has a timer;
 *  - a tick that mt_next_deadline said was not due transmits nothing, and a tick that was due
 *    leaves no expired deadline behind;
 *  - link refusals (2% at random plus bursts of 1..8 refused frames): tcp_tx_stalls equals the
 *    TCP frames the link refused, a stalled TCB always has the stall timer armed, and a refused
 *    new data/FIN segment never counts as sent (snd_max does not pass its sequence number until
 *    the link accepts a frame covering it). */

#define NT 16                 /* TCBs the fuzzer creates (= default max_tcp) */
#define NU 4
#define ND 8                  /* DNS result slots */
#define STEPS 20000
#define NSEEDS 8

#define ME   T_ME
#define PEER T_PEER
#define GW   T_GW

typedef struct {
    uint32_t rng, seed, now;
    long step, bad, ntx;
    int cb_depth, checking_early;
    mt_stack *s;
    mt_tcp *t[NT];
    mt_udp *u[NU];
    mt_dns_result dres[ND];
    int dres_busy[ND];
    mt_dns_result dres_snap[ND];     /* copy taken when cancelled: must never change again */
    int dres_dead[ND];
    char pbuf[48], pbuf_snap[48];    /* reverse lookup name buffer; same cancel rule */
    int pbuf_busy, pbuf_dead;
    uint8_t lastout[8][1514];
    uint16_t lastlen[8];
    int lastn;
    long ev[9];               /* TCP events seen, by mt_tcp_event */
    long done_ok, done_abort, busy, two_queued, inval, reopen_in_done;
    long tcp_refused, stalls_seen, rf_checked;   /* TCP frames the link refused; stalled TCBs seen */
    int refuse_burst;                            /* frames still to refuse in the current burst */
    uint8_t rf_live[NT];                         /* per stack TCB: a refused new segment pending */
    uint32_t rf_seq[NT], rf_iss[NT];
} fz;

/* Send request records: three per fuzzer TCB slot (at most MT_SENDQ_MAX are queued, so one is
 * always free). Segments are laid out in buf at odd and even offsets with canary bytes between
 * them; serial orders the requests of a slot so done order can be checked. */
#define RQ_PER 3
#define RQ_BUF 2400
#define CANARY 0xC3u
typedef struct {
    int busy, slot;
    uint8_t gen;              /* TCB generation at submit: MT_ERR_ABORTED needs a termination since */
    long serial;
    uint8_t nsegs;
    mt_seg segs[MT_SEGS_MAX + 1], segs_copy[MT_SEGS_MAX + 1];
    uint8_t buf[RQ_BUF], copy[RQ_BUF];
} sreq;
static sreq g_rq[16][RQ_PER];
static long g_serial[16], g_next_done[16];

static fz g;
static uint64_t g_arena[16384 / 8];
static uint8_t g_tbuf[NT][8192 + 1];
static uint8_t g_ubuf[NU][2048];

static uint32_t rnd(void) {
    uint32_t x = g.rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return g.rng = x;
}
static uint32_t rn(uint32_t n) { return n ? rnd() % n : 0; }

/* Record a violated property with enough context to replay it. */
static void bad(const char *what) {
    g.bad++;
    if (g.bad <= 10) fprintf(stderr, "fuzz seed %u step %ld: %s\n", (unsigned)g.seed, g.step, what);
}

/* Payload byte for sequence number seq of a connection whose local port is lport. */
static uint8_t spat(uint32_t seq, uint16_t lport) {
    return (uint8_t)(((seq * 2654435761u) >> 13) ^ lport ^ (lport >> 8));
}

/* ---- output verification ---- */

static void verify_l4(const uint8_t *ip, uint16_t tot, mt_ip4 src, mt_ip4 dst) {
    const uint8_t *p = ip + 20;
    uint16_t pl = (uint16_t)(tot - 20);
    uint32_t ps = (src >> 16) + (src & 0xFFFFu) + (dst >> 16) + (dst & 0xFFFFu) + pl;
    if (ip[9] == 6) {
        uint16_t hl = (uint16_t)((p[12] >> 4) << 2), k;
        uint32_t seq = mt_rd32(p + 4);
        if (mt_cksum_fold(mt_cksum_add(ps + 6, p, pl)) != 0) bad("tcp checksum");
        if (mt_rd16(p) == 0 || mt_rd16(p + 2) == 0) bad("tcp port 0");
        if (hl < 20 || hl > pl) { bad("tcp header length"); return; }
        for (k = 0; k < pl - hl; k++)
            if (p[hl + k] != spat(seq + k, mt_rd16(p))) { bad("tcp payload differs from the caller's bytes"); break; }
    } else if (ip[9] == 17) {
        if (mt_rd16(p + 4) != pl) bad("udp length");
        if (mt_rd16(p + 6) && mt_cksum_fold(mt_cksum_add(ps + 17, p, pl)) != 0) bad("udp checksum");
    } else if (ip[9] == 1) {
        if (mt_cksum_fold(mt_cksum_add(0, p, pl)) != 0) bad("icmp checksum");
    }
}

static void verify_out(const uint8_t *f, uint16_t len) {
    const uint8_t *ip = f + 14;
    uint16_t et, tot, ff;
    mt_ip4 src, dst;
    if (len < 14) { bad("frame shorter than an Ethernet header"); return; }
    et = mt_rd16(f + 12);
    if (memcmp(f + 6, g.s->cfg.mac, 6) != 0) bad("ethernet source is not our MAC");
    if (et == 0x0806) {
        if (len != 60) bad("ARP frame is not 60 bytes");
        return;
    }
    if (et != 0x0800) { bad("unknown ethertype"); return; }
    if (len < 34) { bad("IP frame too short"); return; }
    tot = mt_rd16(ip + 2);
    ff = mt_rd16(ip + 6);
    src = mt_rd32(ip + 12);
    dst = mt_rd32(ip + 16);
    if (ip[0] != 0x45 || tot != len - 14) { bad("IP version/length"); return; }
    if (tot > g.s->cfg.mtu) bad("datagram exceeds the MTU");
    if (mt_cksum_fold(mt_cksum_add(0, ip, 20)) != 0) bad("IP header checksum");
    if (src != g.s->cfg.addr) bad("IP source is not our address");
    if (dst == 0 || (dst >= 0xE0000000u && dst != 0xFFFFFFFFu)) bad("multicast or zero destination");
    if ((ff & 0x3FFFu) == 0) verify_l4(ip, tot, src, dst);   /* first fragment or unfragmented */
}

/* The stack TCB that sent TCP frame f (NULL if none matches). */
static int tcb_of_frame(const uint8_t *f, uint16_t len) {
    uint16_t i;
    if (len < 54 || mt_rd16(f + 12) != 0x0800 || f[23] != 6) return -1;
    for (i = 0; i < g.s->lim.max_tcp && i < NT; i++) {
        const mt_tcp *t = &g.s->tcp[i];
        if (t->used && t->lport == mt_rd16(f + 34) && t->rport == mt_rd16(f + 36) &&
            t->rip == mt_rd32(f + 30))
            return (int)i;
    }
    return -1;
}

static int link_tx(void *ctx, const uint8_t *f, uint16_t len) {
    int refuse, k;
    (void)ctx;
    g.ntx++;
    if (g.checking_early) bad("transmit from a tick that was not due");
    verify_out(f, len);
    memcpy(g.lastout[g.lastn & 7], f, len);
    g.lastlen[g.lastn & 7] = len;
    g.lastn++;
    if (g.refuse_burst) {                                   /* tx slots all busy for a while */
        g.refuse_burst--;
        refuse = 1;
    } else {
        refuse = rn(50) == 0;                               /* 2% link errors */
        if (!refuse && rn(300) == 0) g.refuse_burst = (int)rn(8);
    }
    if (len >= 54 && mt_rd16(f + 12) == 0x0800 && f[23] == 6) {
        const uint8_t *th = f + 14 + ((f[14] & 15u) << 2);
        uint32_t seq = mt_rd32(th + 4);
        uint32_t space = (uint32_t)(mt_rd16(f + 16) - ((f[14] & 15u) << 2) - ((th[12] >> 4) << 2)) +
                         ((th[13] & 3u) ? 1u : 0u);        /* payload + SYN/FIN */
        int held = f == g.s->pend->frame;                   /* released from the ARP slot */
        if (refuse && !held) g.tcp_refused++;               /* refusals TCP itself sees */
        k = tcb_of_frame(f, len);
        if (k >= 0 && space && !(held && refuse)) {
            const mt_tcp *t = &g.s->tcp[k];
            if (refuse && !(th[13] & 2u) && seq == t->snd_max && t->state >= MT_TCP_ESTABLISHED &&
                !g.rf_live[k]) {                            /* new data / FIN refused */
                g.rf_live[k] = 1;
                g.rf_seq[k] = seq;
                g.rf_iss[k] = t->iss;
            } else if (!refuse && g.rf_live[k] && mt_seq_le(seq, g.rf_seq[k]) &&
                       mt_seq_lt(g.rf_seq[k], seq + space)) {
                g.rf_live[k] = 0;                           /* accepted at last */
            }
        }
    }
    return refuse;
}

/* ---- callbacks and the actions they take ---- */

static void dns_cb(void *user, const mt_dns_result *r) {
    (void)r;
    if (g.dres_dead[(int)(intptr_t)user]) bad("DNS callback after mt_dns_cancel");
    g.dres_busy[(int)(intptr_t)user] = 0;
}

static void ptr_cb(void *user, mt_dns_status st, const char *name) {
    (void)user; (void)st;
    if (g.pbuf_dead) bad("PTR callback after mt_dns_cancel");
    if (name != g.pbuf || strlen(name) >= sizeof g.pbuf) bad("PTR callback with a bad name");
    g.pbuf_busy = 0;
}

static void start_ptr(void) {
    int r;
    if (g.pbuf_busy) return;
    memset(g.pbuf, 'q', sizeof g.pbuf);
    r = mt_dns_ptr(g.s, rn(3) == 0 ? PEER : rnd(), g.pbuf, (uint16_t)(1 + rn(sizeof g.pbuf)), ptr_cb, NULL, g.now);
    g.pbuf_dead = 0;
    if (r == MT_DNS_IN_PROGRESS) g.pbuf_busy = 1;
}

/* Cancel one of the pending lookups and remember the target's bytes: it must never change. */
static void cancel_dns(void) {
    int i = (int)rn(ND + 1);
    if (i == ND) {
        if (!g.pbuf_busy) return;
        mt_dns_cancel(g.s, g.pbuf);
        memcpy(g.pbuf_snap, g.pbuf, sizeof g.pbuf);
        g.pbuf_busy = 0; g.pbuf_dead = 1;
    } else {
        if (!g.dres_busy[i]) return;
        mt_dns_cancel(g.s, &g.dres[i]);
        g.dres_snap[i] = g.dres[i];
        g.dres_busy[i] = 0; g.dres_dead[i] = 1;
    }
}

static void check_cancelled(void) {
    int i;
    for (i = 0; i < ND; i++)
        if (g.dres_dead[i] && memcmp(&g.dres[i], &g.dres_snap[i], sizeof g.dres[i]) != 0)
            bad("DNS result written after mt_dns_cancel");
    if (g.pbuf_dead && memcmp(g.pbuf, g.pbuf_snap, sizeof g.pbuf) != 0)
        bad("PTR buffer written after mt_dns_cancel");
}

static void start_dns(void) {
    static const char *const names[] = {"wiki", "en.wikipedia.org", "1.2.3.4", "x.", "a..b", "host",
        "verylonglabel-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.com"};
    int i, r;
    mt_dns_qtype q = rn(2) ? MT_DNS_Q_A : (rn(2) ? MT_DNS_Q_PTR : MT_DNS_Q_MX);
    for (i = 0; i < ND; i++) if (!g.dres_busy[i]) break;
    if (i == ND) return;
    g.dres_dead[i] = 0;
    r = mt_dns_query(g.s, names[rn(7)], q, &g.dres[i], dns_cb, (void *)(intptr_t)i, g.now);
    if (r == MT_DNS_IN_PROGRESS) g.dres_busy[i] = 1;
}

static int tslot(const mt_tcp *t) {
    int i;
    for (i = 0; i < NT; i++) if (g.t[i] == t) return i;
    return -1;
}

/* The request's segments, canaries and segment array must be exactly as submitted. */
static void rq_verify(const sreq *r) {
    if (memcmp(r->buf, r->copy, RQ_BUF) != 0) bad("send request memory was written");
    if (memcmp(r->segs, r->segs_copy, sizeof r->segs) != 0) bad("mt_seg array was written");
}

static void rq_release(sreq *r) {
    rq_verify(r);
    r->busy = 0;
}

static void tcp_action(mt_tcp *t);

static void send_done(void *ud, int st) {
    sreq *r = (sreq *)ud;
    mt_tcp *t;
    if (!r->busy) { bad("done for a request that is not pending"); return; }
    if (r->serial != g_next_done[r->slot]) bad("send done out of submission order");
    g_next_done[r->slot] = r->serial + 1;
    t = g.t[r->slot];
    if (st == MT_OK) g.done_ok++;
    else if (st == MT_ERR_ABORTED) {
        g.done_abort++;                                     /* only after a termination (gen bump) */
        if (t && t->used && t->gen == r->gen) bad("send aborted without a termination");
    } else bad("send done with an unknown status");
    rq_release(r);
    if (t && t->used && g.cb_depth <= 3) {
        g.cb_depth++;
        if (t->state == MT_TCP_CLOSED && rn(2) == 0) {      /* re-use the stream from inside done */
            g.reopen_in_done++;
            if (rn(2)) mt_tcp_listen(t, 0, 0, (uint16_t)(rn(2) ? 23 : 0));
            else mt_tcp_open(t, PEER, (uint16_t)(80 + rn(3)), 0, g.now);
        } else if (rn(3) == 0) {                            /* re-enter from the done callback */
            tcp_action(t);
        }
        g.cb_depth--;
    }
}

/* A random send request: 1..8 segments (sometimes 0 or 9 to be refused), lengths from 0 to 1460
 * (odd ones, empty ones), at odd/even offsets with canaries between them, filled with the payload
 * pattern for the sequence numbers they will occupy. */
static void fuzz_send(mt_tcp *t) {
    int slot = tslot(t), k, r;
    sreq *q = NULL;
    uint32_t pos, total = 0, seq0;
    uint8_t ns, i;
    int valid_state = t->state == MT_TCP_ESTABLISHED || t->state == MT_TCP_CLOSE_WAIT;
    if (slot < 0) return;
    for (k = 0; k < RQ_PER; k++) if (!g_rq[slot][k].busy) { q = &g_rq[slot][k]; break; }
    if (!q) { bad("no free send record (more than MT_SENDQ_MAX pending)"); return; }
    ns = (uint8_t)(rn(12) == 0 ? (rn(2) ? 0 : MT_SEGS_MAX + 1) : 1 + rn(MT_SEGS_MAX));
    memset(q->buf, CANARY, RQ_BUF);
    memset(q->segs, 0, sizeof q->segs);
    seq0 = t->snd_una + mt_tcp_sq_total(t);
    pos = rn(2);
    for (i = 0; i < ns; i++) {
        uint32_t len = rn(4) == 0 ? 0 : rn(8) == 0 ? 1460 : rn(300), j;
        pos += rn(3);                                       /* canary gap (0..2 bytes) */
        if (pos + len > RQ_BUF) len = pos < RQ_BUF ? RQ_BUF - pos : 0;
        q->segs[i].p = len || rn(2) ? q->buf + pos : NULL;  /* empty entries may be NULL */
        q->segs[i].len = (uint16_t)len;
        for (j = 0; j < len; j++) q->buf[pos + j] = spat(seq0 + total + j, t->lport);
        pos += len;
        total += len;
    }
    memcpy(q->copy, q->buf, RQ_BUF);
    memcpy(q->segs_copy, q->segs, sizeof q->segs);
    q->busy = 1;                                            /* before the call: done may not fire */
    q->slot = slot;
    q->nsegs = ns;
    q->serial = g_serial[slot];
    q->gen = t->gen;
    k = t->sq_n;
    r = mt_tcp_send_segs(t, q->segs, ns, (int)rn(2), rn(10) == 0, send_done, q, g.now);
    if (ns == 0 || ns > MT_SEGS_MAX || total == 0) {
        if (r != MT_ERR_INVAL) bad("invalid send request not refused with MT_ERR_INVAL");
        g.inval++;
    } else if (!valid_state) {
        if (r != MT_ERR_STATE) bad("send in a non-sending state not refused with MT_ERR_STATE");
    } else if (k >= MT_SENDQ_MAX) {
        if (r != MT_ERR_BUSY) bad("third send request not refused with MT_ERR_BUSY");
        g.busy++;
    } else if (r != MT_OK) {
        bad("valid send request refused");
    }
    if (r != MT_OK) {
        rq_release(q);
        return;
    }
    g_serial[slot]++;
    if (t->used && t->sq_n == MT_SENDQ_MAX) g.two_queued++;
}

/* Release: the requests still queued in the TCB are dropped silently (the caller owns them);
 * requests a terminating TCB already detached keep their pending done calls. */
static void drop_tcb(mt_tcp *t) {
    int i = tslot(t), k;
    uint8_t r;
    if (i >= 0)
        for (k = 0; k < RQ_PER; k++)
            for (r = 0; r < t->sq_n; r++)
                if (g_rq[i][k].busy && t->sq[r].segs == g_rq[i][k].segs) rq_release(&g_rq[i][k]);
    mt_tcp_release(t);
    if (i >= 0) {
        long next = g_serial[i];
        g.t[i] = NULL;
        for (k = 0; k < RQ_PER; k++)
            if (g_rq[i][k].busy && g_rq[i][k].serial < next) next = g_rq[i][k].serial;
        g_next_done[i] = next;
    }
}

/* One random API call on a live TCB (also used from inside callbacks). */
static void tcp_action(mt_tcp *t) {
    uint8_t b[600];
    memset(b, 'u', sizeof b);
    switch (rn(20)) {
    case 0: case 1: case 2: case 14:
        fuzz_send(t);
        break;
    case 3: case 4: case 5: mt_tcp_recv(t, b, rn(sizeof b), g.now); break;
    case 6: {
        const uint8_t *p;
        uint32_t n = mt_tcp_peek(t, 0, &p);
        if (n) mt_tcp_consume(t, rn(n + 1), g.now);
        break;
    }
    case 7: if (rn(4) == 0) mt_tcp_close(t, g.now); break;
    case 8: if (rn(8) == 0) mt_tcp_abort(t, g.now); break;
    case 9: if (rn(10) == 0) drop_tcb(t); break;
    case 10:
        if (rn(4) == 0) {
            switch (rn(4)) {
            case 0: case 1: start_dns(); break;
            case 2: start_ptr(); break;
            default: cancel_dns(); break;
            }
        }
        break;
    case 11: if (g.u[0] && rn(3) == 0) mt_udp_send(g.u[0], PEER, 9, b, (uint16_t)rn(300), 1, g.now); break;
    case 12: if (rn(4) == 0) mt_icmp_echo(g.s, PEER, 1, 2, b, (uint16_t)rn(100), g.now); break;
    case 13: { mt_tcp_info info; mt_tcp_get_info(t, &info); break; }
    default: break;
    }
}

static void tcp_cb(mt_tcp *t, mt_tcp_event ev, uint32_t arg, void *user) {
    int n, i;
    (void)arg; (void)user;
    if ((int)ev < 9) g.ev[ev]++;
    if (!t->used) { bad("callback on a freed TCB"); return; }
    if (g.cb_depth > 3) return;
    g.cb_depth++;
    if (ev == MT_TCP_EV_TERMINATE) {
        if (rn(3) == 0) drop_tcb(t);
        else if (rn(2) == 0) mt_tcp_listen(t, 0, 0, (uint16_t)(rn(2) ? 23 : 0));
    } else {
        n = (int)rn(3);
        for (i = 0; i < n && t->used; i++) tcp_action(t);   /* an action may release t */
    }
    g.cb_depth--;
}

static void udp_cb(mt_udp *u, mt_udp_event ev, uint32_t arg, void *user) {
    mt_udp_dgram d;
    uint8_t b[64];
    int i = (int)(intptr_t)user;
    (void)arg;
    if (ev != MT_UDP_EV_DATA) return;
    memset(b, 0, sizeof b);
    switch (rn(5)) {
    case 0: while (mt_udp_peek(u, &d)) mt_udp_done(u); break;
    case 1:                                                 /* echo it back, then drop it */
        if (mt_udp_peek(u, &d)) mt_udp_send(u, d.src, d.sport, d.data, d.len > 400 ? 400 : d.len, 1, g.now);
        mt_udp_done(u);
        break;
    case 2: if (rn(5) == 0) { mt_udp_release(u); g.u[i] = NULL; } break;
    case 3:
        if (g.cb_depth < 3) {
            g.cb_depth++;
            start_dns();
            mt_udp_send(u, PEER, 7, b, 10, 0, g.now);
            g.cb_depth--;
        }
        break;
    default: break;
    }
}

static void echo_cb(void *user, mt_ip4 from, uint16_t id, uint16_t seq, const uint8_t *data,
                    uint16_t len, uint32_t now) {
    (void)user; (void)from; (void)id; (void)seq; (void)data; (void)len; (void)now;
    if (rn(3) == 0) mt_icmp_echo(g.s, PEER, 9, 9, (const uint8_t *)"abc", 3, g.now);
}

static void ensure_objects(void) {
    int i;
    for (i = 0; i < NT; i++) {
        mt_tcp *t = g.t[i];
        if (!t && rn(4) == 0)                               /* any size >= 512, odd address */
            t = g.t[i] = mt_tcp_create(g.s, g_tbuf[i] + rn(2), 512 + rn(8192 - 512), tcp_cb, NULL);
        if (t && t->state == MT_TCP_CLOSED && rn(8) == 0) {
            if (rn(2)) mt_tcp_listen(t, rn(4) ? 0 : PEER, 0, (uint16_t)(rn(2) ? 23 : 21));
            else mt_tcp_open(t, rn(8) ? PEER : T_FAR, (uint16_t)(80 + rn(3)),
                             rn(4) ? 0 : (uint16_t)(1024 + rn(4)), g.now);
        }
    }
    for (i = 0; i < NU; i++)
        if (!g.u[i] && rn(8) == 0)
            g.u[i] = mt_udp_create(g.s, (uint16_t)(i == 0 ? 7 : rn(2) ? 0 : 1000 + rn(3)), g_ubuf[i],
                                   1024 + rn(1024), udp_cb, (void *)(intptr_t)i);
}

/* ---- invariants ---- */

static void check_sendq(const mt_tcp *t) {
    uint32_t q = mt_tcp_sq_total(t), left = 0;
    uint8_t r, k;
    if (t->sq_n > MT_SENDQ_MAX) { bad("send queue overflow"); return; }
    for (r = 0; r < t->sq_n; r++) {
        if (t->sq[r].left == 0) bad("completed send request left un-notified");
        left += t->sq[r].left;
    }
    if (left != q) bad("send queue byte count");
    if (t->sq_n) {             /* cursor = the byte at snd_una (possibly after empty entries) */
        const mt_sendreq *h = &t->sq[0];
        uint32_t rest = 0;
        uint8_t ci = t->sq_idx;
        if (t->sq_off == 0)
            while (ci < h->nsegs && h->segs[ci].len == 0) ci++;
        if (ci >= h->nsegs || t->sq_off >= h->segs[ci].len) {
            bad("send cursor not on a byte");
            return;
        }
        for (k = ci; k < h->nsegs; k++) rest += h->segs[k].len;
        if (rest - t->sq_off != h->left) bad("send cursor and bytes left disagree");
        if (h->segs[ci].p[t->sq_off] != spat(t->snd_una, t->lport))
            bad("send cursor does not point at the byte at snd_una");
    }
    if (t->sq_n == 0 && (t->sq_idx || t->sq_off)) bad("send cursor not reset");
}

/* 1 if frames to rip could have been held or dropped for ARP (no resolved entry for its next hop). */
static int arp_unresolved(mt_ip4 rip) {
    mt_ip4 nh = ((rip ^ g.s->cfg.addr) & g.s->cfg.mask) == 0 ? rip : g.s->cfg.gateway;
    uint16_t i;
    for (i = 0; i < g.s->lim.arp_entries; i++)
        if (g.s->arp[i].ip == nh && g.s->arp[i].state == MT_ARP_RESOLVED) return 0;
    return 1;
}

static void check_invariants(void) {
    const uint8_t rp = (1u << MT_TT_RTO) | (1u << MT_TT_PERSIST);
    uint16_t i;
    int k, n;
    for (i = 0; i < NT; i++) {                              /* every pending record is queued */
        for (k = 0, n = 0; k < RQ_PER; k++) n += g_rq[i][k].busy;
        if (g.t[i] && g.t[i]->used ? n != g.t[i]->sq_n : n != 0)
            bad("pending send records differ from the TCB's queue");
    }
    for (i = 0; i < g.s->lim.max_tcp; i++) {
        const mt_tcp *t = &g.s->tcp[i];
        if (!t->used) continue;
        if (t->rcvq.used > t->rcvq.size) bad("ring overflow");
        check_sendq(t);
        if (t->sq_n && t->state != MT_TCP_ESTABLISHED && t->state != MT_TCP_CLOSE_WAIT &&
            t->state != MT_TCP_FIN_WAIT_1 && t->state != MT_TCP_CLOSING &&
            t->state != MT_TCP_LAST_ACK)
            bad("send requests queued in a state that cannot send them");
        if (t->state >= MT_TCP_ESTABLISHED) {
            if (mt_seq_gt(t->snd_una, t->snd_nxt) || mt_seq_gt(t->snd_nxt, t->snd_max))
                bad("snd_una <= snd_nxt <= snd_max violated");
            if (t->snd_max - t->snd_una > mt_tcp_sq_total(t) + 1u)
                bad("more unacked than the send queue holds");
        }
        if ((t->tmask & rp) == rp) bad("RTO and PERSIST both armed");
        if (t->state >= MT_TCP_ESTABLISHED && t->state != MT_TCP_TIME_WAIT &&
            t->state != MT_TCP_FIN_WAIT_2 && t->snd_max != t->snd_una && !(t->tmask & rp))
            bad("unacked data without a timer");
        if ((t->flags2 & MT_T2_GAP) && t->state < MT_TCP_ESTABLISHED) bad("gap flag outside a connection");
        if (t->flags2 & MT_T2_STALL) {
            g.stalls_seen++;
            if (!g.s->stall_on) bad("stalled TCB without the stall timer");
            else if (mt_next_deadline(g.s, g.now) > MT_TCP_STALL_MS) bad("stall retry later than 10 ms");
        }
        if (i < NT && g.rf_live[i]) {
            if (t->iss != g.rf_iss[i] || t->state < MT_TCP_ESTABLISHED || t->state == MT_TCP_TIME_WAIT) {
                g.rf_live[i] = 0;                           /* another connection now */
            } else if (mt_seq_gt(t->snd_max, g.rf_seq[i]) && !arp_unresolved(t->rip)) {
                bad("a refused segment was counted as sent");
            } else {
                g.rf_checked++;
            }
        }
    }
    if (g.s->stats.tcp_tx_stalls != (uint32_t)g.tcp_refused) bad("tcp_tx_stalls differs from refused TCP frames");
}

/* Calling mt_tick whenever the deadline reads 0 must clear it. */
static void deadline_check(void) {
    int k;
    for (k = 0; k < 6; k++) {
        if (mt_next_deadline(g.s, g.now) != 0) return;
        mt_tick(g.s, g.now);
    }
    bad("expired deadline survives mt_tick");
}

/* ---- frame generation ---- */

/* Fix the TCP checksum of the frame after the urgent pointer has been rewritten. */
static void set_urgent(uint8_t *f, uint16_t n, mt_ip4 src) {
    uint8_t *tcp = f + 34;
    uint16_t tl = (uint16_t)(n - 34);
    uint32_t sum = (src >> 16) + (src & 0xFFFFu) + (ME >> 16) + (ME & 0xFFFFu) + 6u + tl;
    mt_wr16(tcp + 18, (uint16_t)rn(2000));
    mt_wr16(tcp + 16, 0);
    mt_wr16(tcp + 16, mt_cksum_fold(mt_cksum_add(sum, tcp, tl)));
}

/* A TCP segment from the peer to a random TCB: sequence and ack numbers are mostly near the
 * TCB's live values, sometimes random. */
static uint16_t gen_tcp(uint8_t *f) {
    static uint8_t data[1460];
    uint8_t opt[8], fl;
    uint8_t optlen = 0;
    const mt_tcp *t = &g.s->tcp[rn(g.s->lim.max_tcp)];
    uint32_t seq, ack;
    uint16_t sport, dport, dlen, i, n;
    mt_ip4 src = PEER;
    if (t->used && t->state > MT_TCP_LISTEN) {
        src = t->rip; sport = t->rport; dport = t->lport;
        switch (rn(6)) {
        case 0: case 1: case 2: seq = t->rcv_nxt; break;
        case 3: seq = t->rcv_nxt + rn(3000); break;
        case 4: seq = t->rcv_nxt - rn(3000); break;
        default: seq = rnd(); break;
        }
        switch (rn(6)) {
        case 0: case 1: ack = t->snd_max; break;
        case 2: ack = t->snd_una + rn(t->snd_max - t->snd_una + 1); break;
        case 3: ack = t->snd_una; break;
        case 4: ack = t->snd_una - rn(2000); break;
        default: ack = rnd(); break;
        }
    } else {
        sport = (uint16_t)(1024 + rn(8));
        dport = (uint16_t)(rn(2) ? 23 : 21 + rn(3));
        seq = rnd(); ack = rnd();
    }
    switch (rn(8)) {
    case 0: fl = TF_SYN; break;
    case 1: fl = TF_SYN | TF_ACK; break;
    case 2: fl = (uint8_t)(TF_RST | (rn(2) ? TF_ACK : 0)); break;
    case 3: fl = (uint8_t)(TF_ACK | TF_FIN | (rn(2) ? TF_PSH : 0)); break;
    default: fl = (uint8_t)(TF_ACK | (rn(2) ? TF_PSH : 0) | (rn(8) == 0 ? TF_URG : 0)); break;
    }
    if (rn(30) == 0) fl = (uint8_t)rnd();
    dlen = (uint16_t)(rn(3) == 0 ? 0 : rn(4) == 0 ? 1460 : rn(600));
    if (fl & TF_SYN) dlen = rn(10) == 0 ? (uint16_t)rn(20) : 0;
    if (rn(3) == 0 || (fl & TF_SYN)) {
        opt[0] = 2; opt[1] = 4;
        mt_wr16(opt + 2, (uint16_t)(rn(4) == 0 ? rn(2000) : 1460));
        optlen = 4;
        if (rn(10) == 0) {                                  /* a second, random option */
            opt[4] = (uint8_t)rnd(); opt[5] = (uint8_t)rnd(); opt[6] = 1; opt[7] = 0;
            optlen = 8;
        }
    }
    if (dlen + optlen > 1460) dlen = (uint16_t)(1460 - optlen);
    for (i = 0; i < dlen; i++) data[i] = (uint8_t)(seq + i);
    n = mk_tcp(f, src, sport, ME, dport, seq, ack, fl,
               (uint16_t)(rn(5) == 0 ? 0 : rn(3) == 0 ? rn(65536) : 8192),
               optlen ? opt : NULL, optlen, data, dlen);
    if (fl & TF_URG) set_urgent(f, n, src);
    return n;
}

/* A DNS response to a pending (or imaginary) query: well formed, then a few random byte hits. */
static uint16_t gen_dns(uint8_t *f) {
    uint8_t p[600], u[620];
    uint16_t n = 12, i;
    mt_dns_q *q = &g.s->dns->q[rn(MT_DNS_SLOTS)];
    char name[300];
    const char *s;
    uint32_t sum;
    if (!g.s->dns->udp) return 0;
    mt_wr16(p, q->used ? q->id : (uint16_t)rnd());
    mt_wr16(p + 2, (uint16_t)(0x8180 | (rn(6) == 0 ? 3 : 0)));
    mt_wr16(p + 4, 1); mt_wr16(p + 6, (uint16_t)rn(4)); mt_wr16(p + 8, 0); mt_wr16(p + 10, 0);
    strcpy(name, q->name);
    if (q->dom && g.s->cfg.domain[0]) { strcat(name, "."); strcat(name, g.s->cfg.domain); }
    for (s = name; *s && n < 300;) {                        /* the question, label by label */
        const char *dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        p[n++] = (uint8_t)l;
        memcpy(p + n, s, l);
        n = (uint16_t)(n + l);
        s += l;
        if (*s == '.') s++;
    }
    p[n++] = 0;
    mt_wr16(p + n, q->qtype ? q->qtype : 1); mt_wr16(p + n + 2, 1);
    n = (uint16_t)(n + 4);
    for (i = 0; i < mt_rd16(p + 6) && n < 560; i++) {      /* answers named by a pointer to the question */
        p[n++] = 0xC0; p[n++] = 12;
        if (rn(3) == 0) {                                   /* CNAME with a random compression pointer */
            mt_wr16(p + n, 5); mt_wr16(p + n + 2, 1); mt_wr32(p + n + 4, rnd()); mt_wr16(p + n + 8, 4);
            n = (uint16_t)(n + 10);
            p[n++] = 1; p[n++] = 'z'; p[n++] = 0xC0;
            p[n] = (uint8_t)rn(n);
            n++;
        } else {
            mt_wr16(p + n, q->qtype == 15 ? 15 : q->qtype == 12 ? 12 : 1); mt_wr16(p + n + 2, 1);
            mt_wr32(p + n + 4, rn(100000)); mt_wr16(p + n + 8, 4);
            n = (uint16_t)(n + 10);
            mt_wr32(p + n, rnd());
            n = (uint16_t)(n + 4);
        }
    }
    for (i = (uint16_t)rn(4); i > 0; i--) p[rn(n)] = (uint8_t)rnd();
    mt_wr16(u, 53); mt_wr16(u + 2, mt_udp_port(g.s->dns->udp)); mt_wr16(u + 4, (uint16_t)(8 + n));
    mt_wr16(u + 6, 0);
    memcpy(u + 8, p, n);
    sum = (PEER >> 16) + (PEER & 0xFFFFu) + (ME >> 16) + (ME & 0xFFFFu) + 17u + 8u + n;
    mt_wr16(u + 6, mt_cksum_fold(mt_cksum_add(sum, u, 8u + n)));
    return mk_ip(f, PEER, ME, 17, (uint16_t)rnd(), 0, u, (uint16_t)(8 + n));
}

static uint16_t gen_udp(uint8_t *f) {
    uint8_t pl[1480];
    uint16_t dl = (uint16_t)rn(1200), i;
    mt_wr16(pl, (uint16_t)rn(2000));
    mt_wr16(pl + 2, (uint16_t)(rn(2) ? 7 : rn(4) ? 1000 + rn(3) : rn(65536)));
    mt_wr16(pl + 4, (uint16_t)(8 + dl));
    mt_wr16(pl + 6, 0);
    for (i = 0; i < dl; i++) pl[8 + i] = (uint8_t)rnd();
    if (rn(2)) {                                            /* else checksum 0: "none" */
        uint32_t sum = (PEER >> 16) + (PEER & 0xFFFFu) + (ME >> 16) + (ME & 0xFFFFu) + 17u + 8u + dl;
        mt_wr16(pl + 6, mt_cksum_fold(mt_cksum_add(sum, pl, 8u + dl)));
    }
    return mk_ip(f, PEER, rn(10) ? ME : 0xFFFFFFFFu, 17, (uint16_t)rnd(), 0, pl, (uint16_t)(8 + dl));
}

/* ICMP: an echo request/reply, or a destination-unreachable/time-exceeded quoting one of our
 * connections. */
static uint16_t gen_icmp(uint8_t *f) {
    uint8_t pl[1480];
    uint16_t l, i;
    if (rn(2)) {
        l = (uint16_t)(8 + rn(600));
        for (i = 0; i < l; i++) pl[i] = (uint8_t)rnd();
        pl[0] = (uint8_t)(rn(2) ? 8 : 0);
        pl[1] = 0;
    } else {
        const mt_tcp *t = &g.s->tcp[rn(g.s->lim.max_tcp)];
        l = 8 + 28;
        memset(pl, 0, l);
        pl[0] = (uint8_t)(rn(2) ? 3 : 11);
        pl[1] = (uint8_t)rn(5);
        pl[8] = 0x45;
        pl[8 + 9] = 6;
        mt_wr32(pl + 8 + 12, ME);
        mt_wr32(pl + 8 + 16, t->rip);
        mt_wr16(pl + 28, t->lport);
        mt_wr16(pl + 30, t->rport);
    }
    mt_wr16(pl + 2, 0);
    mt_wr16(pl + 2, mt_cksum_fold(mt_cksum_add(0, pl, l)));
    return mk_ip(f, PEER, ME, 1, (uint16_t)rnd(), 0, pl, l);
}

static uint16_t gen_fragment(uint8_t *f) {
    uint8_t pl[1480];
    uint16_t off = (uint16_t)rn(600), l = (uint16_t)(8 * (1 + rn(100))), i;
    for (i = 0; i < l; i++) pl[i] = (uint8_t)rnd();
    pl[0] = 8;
    pl[1] = 0;
    return mk_ip(f, PEER, ME, (uint8_t)(rn(2) ? 1 : 17), (uint16_t)rn(3),
                 (uint16_t)((rn(2) ? 0x2000 : 0) | off), pl, l);
}

static uint16_t gen_frame(uint8_t *f) {
    uint16_t n;
    switch (rn(14)) {
    case 0: return mk_arp(f, (uint16_t)(1 + rn(2)), rn(2) ? T_PEER_MAC : T_GW_MAC,
                          rn(2) ? PEER : GW, g.s->cfg.mac, ME);
    case 1: case 2: return gen_udp(f);
    case 3: return gen_icmp(f);
    case 4: return gen_fragment(f);
    case 5: n = gen_dns(f); return n ? n : gen_tcp(f);
    default: return gen_tcp(f);
    }
}

/* Does the (possibly damaged) frame carry a TCP segment whose checksum verifies, read the way
 * the core reads it (IHL and total length from the header)? Returns the offset of the TCP
 * checksum field, or 0. */
static uint32_t tcp_cksum_verifies(const uint8_t *f, uint16_t n) {
    const uint8_t *ip = f + 14;
    uint32_t ihl, tot, src, dst, sum;
    if (n < 34 || mt_rd16(f + 12) != 0x0800 || ip[9] != 6) return 0;
    ihl = (uint32_t)(ip[0] & 15) << 2;
    tot = mt_rd16(ip + 2);
    if (ihl < 20 || tot > (uint32_t)n - 14 || tot < ihl + 20) return 0;
    src = mt_rd32(ip + 12);
    dst = mt_rd32(ip + 16);
    sum = (src >> 16) + (src & 0xFFFFu) + (dst >> 16) + (dst & 0xFFFFu) + 6u + (tot - ihl);
    if (mt_cksum_fold(mt_cksum_add(sum, ip + ihl, (uint16_t)(tot - ihl))) != 0) return 0;
    return 14 + ihl + 16;
}

/* Did the damage touch anything the TCP checksum covers (pseudo-header fields: header length,
 * total length, protocol, addresses; and everything after the 20-byte IP header)? Changes to
 * TOS, ID, flags/offset or TTL alone leave the segment intact and its checksum rightly valid. */
static int tcp_visible_change(const uint8_t *a, const uint8_t *b, uint16_t n) {
    const uint8_t *x = a + 14, *y = b + 14;
    return (x[0] & 15) != (y[0] & 15) || x[2] != y[2] || x[3] != y[3] || x[9] != y[9] ||
           memcmp(x + 12, y + 12, 8) != 0 || memcmp(x + 20, y + 20, (size_t)n - 34) != 0;
}

/* Flip 1..4 bytes of the IP packet; half the time repair the IP header checksum so the damage
 * reaches the upper layers. The flips are random, so with two or more of them the TCP checksum
 * can still verify (the ones-complement sum of the changes cancels, about 1 in 65535): damage
 * TCP cannot detect by design, which the receive-data oracle (rx_check) must never see. Such a
 * frame gets one more bit flipped in its TCP checksum, so every damaged segment is detectable. */
static void mutate(uint8_t *f, uint16_t n) {
    static uint8_t orig[1600];
    uint32_t k, m = 1 + rn(4), hl, ck;
    memcpy(orig, f, n < sizeof orig ? n : sizeof orig);
    for (k = 0; k < m; k++) f[14 + rn((uint32_t)n - 14)] ^= (uint8_t)(1 + rn(255));
    if (rn(2)) {
        hl = (uint32_t)(f[14] & 15) << 2;
        if (hl > (uint32_t)n - 14) hl = 20;
        mt_wr16(f + 24, 0);
        mt_wr16(f + 24, mt_cksum_fold(mt_cksum_add(0, f + 14, hl)));
    }
    if (n <= sizeof orig && tcp_visible_change(orig, f, n) && (ck = tcp_cksum_verifies(f, n)) != 0)
        f[ck + 1] ^= 1u;
}

/* ---- the run ---- */

static void fuzz_setup(uint32_t seed) {
    static const uint16_t mtus[] = {1500, 576, 68, 296, 1500, 1500};
    mt_limits lim;
    mt_link link;
    mt_ifconfig c;
    memset(&g, 0, sizeof g);
    memset(g_rq, 0, sizeof g_rq);
    memset(g_serial, 0, sizeof g_serial);
    memset(g_next_done, 0, sizeof g_next_done);
    g.seed = seed;
    g.rng = seed * 2654435761u + 1u;
    link.ctx = NULL;
    link.tx = link_tx;
    mt_limits_default(&lim);
    g.s = mt_stack_init(g_arena, sizeof g_arena, &lim, &link, rnd());
    T_CHECK(g.s != NULL);
    if (!g.s) return;
    memset(&c, 0, sizeof c);
    c.addr = ME; c.mask = 0xFFFFFF00u; c.gateway = GW; c.dns = PEER;
    if (rn(2)) strcpy(c.domain, "example.com");
    memcpy(c.mac, T_ME_MAC, 6);
    c.mtu = mtus[rn(6)];
    mt_set_config(g.s, &c);
    mt_set_echo_cb(g.s, echo_cb, NULL);
    mt_log_enable(g.s, 1);
    g.now = rn(2) ? 0xFFF00000u + rnd() % 0x100000u : rnd();   /* half the runs start near the clock wrap */
}

static void fuzz_step(uint8_t *f) {
    uint32_t d, dt;
    ensure_objects();
    if (rn(3)) {
        uint16_t n = gen_frame(f);
        if (rn(20) == 0 && n > 14) mutate(f, n);
        if (rn(4) == 0) {                                   /* odd alignment */
            static uint8_t odd[1520];
            memcpy(odd + 1, f, n);
            mt_input(g.s, odd + 1, n, g.now);
        } else {
            mt_input(g.s, f, n, g.now);
        }
    }
    if (rn(8) == 0 && g.lastn) {                            /* reflect one of our own frames back */
        int k = (int)rn(g.lastn < 8 ? (uint32_t)g.lastn : 8u);
        mt_input(g.s, g.lastout[k], g.lastlen[k], g.now);
    }
    if (rn(6) == 0) {
        int i = (int)rn(NT);
        if (g.t[i] && g.t[i]->used) tcp_action(g.t[i]);
    }
    if (rn(200) == 0) {                                     /* MTU or domain change under load */
        mt_ifconfig c2;
        mt_get_config(g.s, &c2);
        c2.mtu = (uint16_t)(rn(2) ? 576 : 1500);
        if (rn(4) == 0) c2.domain[0] = 0;
        mt_set_config(g.s, &c2);
    }
    if (rn(500) == 0) { mt_logrec lr[16]; mt_log_read(g.s, lr, 16); }
    check_invariants();
    check_cancelled();
    deadline_check();
    d = mt_next_deadline(g.s, g.now);                       /* early tick: nothing due, nothing sent */
    if (d > 0 && rn(4) == 0) {
        g.checking_early = 1;
        mt_tick(g.s, g.now);
        g.checking_early = 0;
    }
    dt = rn(10) == 0 ? rn(5000) : rn(150);
    if (rn(2000) == 0) dt = 600000u + rn(100000);
    if (d != MT_NO_DEADLINE && d < dt && rn(2)) {           /* land exactly on the deadline first */
        g.now += d;
        deadline_check();
        dt -= d;
    }
    g.now += dt;
    if (mt_next_deadline(g.s, g.now) == 0) mt_tick(g.s, g.now);
}

static void fuzz_seed(uint32_t seed) {
    static uint8_t f[1514];
    fuzz_setup(seed);
    if (!g.s) return;
    for (g.step = 0; g.step < STEPS && g.bad == 0; g.step++) fuzz_step(f);
    T_EQ(g.bad, 0);
    if (g.bad) fprintf(stderr, "fuzz seed %u failed at step %ld\n", (unsigned)seed, g.step);
}

static void test_fuzz_seeds(void) {
    uint32_t seed;
    long est = 0, data = 0, term = 0, tx = 0, dok = 0, dab = 0, busy = 0, two = 0, inval = 0;
    long reopen = 0, refused = 0, stalled = 0, rfc = 0;
    /* FZ_SEED=n runs only seed n (replay a failure); FZ_SEEDS=n runs seeds 1..n (default NSEEDS) */
    uint32_t first = getenv("FZ_SEED") ? (uint32_t)atoi(getenv("FZ_SEED")) : 1;
    uint32_t last = getenv("FZ_SEED") ? first : getenv("FZ_SEEDS") ? (uint32_t)atoi(getenv("FZ_SEEDS")) : NSEEDS;
    for (seed = first; seed <= last; seed++) {
        fuzz_seed(seed);
        refused += g.tcp_refused; stalled += g.stalls_seen; rfc += g.rf_checked;
        est += g.ev[MT_TCP_EV_ESTABLISHED]; data += g.ev[MT_TCP_EV_DATA];
        term += g.ev[MT_TCP_EV_TERMINATE]; tx += g.ntx;
        dok += g.done_ok; dab += g.done_abort; busy += g.busy; two += g.two_queued;
        reopen += g.reopen_in_done;
        inval += g.inval;
    }
    if (getenv("FUZZ_VERBOSE"))
        fprintf(stderr, "est %ld data %ld term %ld tx %ld done ok %ld abort %ld busy %ld two %ld inval %ld"
                " reopen %ld; tcp refused %ld stalled-TCB checks %ld refused-segment checks %ld\n",
                est, data, term, tx, dok, dab, busy, two, inval, reopen, refused, stalled, rfc);
    /* The fuzzer must actually exercise the stack: connections come up, carry data and die;
     * sends complete and abort, the queue fills to two and refuses a third. */
    T_CHECK(est > 0);
    T_CHECK(data > 0);
    T_CHECK(term > 0);
    T_CHECK(tx > 1000);
    T_CHECK(dok > 0);
    T_CHECK(dab > 0);
    T_CHECK(two > 0);
    T_CHECK(busy > 0);
    T_CHECK(inval > 0);
    T_CHECK(reopen > 0);
    T_CHECK(refused > 0);       /* the link refused TCP frames, TCBs stalled and were checked */
    T_CHECK(stalled > 0);
    T_CHECK(rfc > 0);
}

/* A long run whose clock starts just below the 32-bit wrap (the seed picks near-wrap starts for
 * about half the runs; this one is forced by trying seeds until one does). */
static void test_fuzz_clock_wrap(void) {
    uint32_t seed;
    int found = 0;
    for (seed = 100; seed < 120 && !found; seed++) {
        fuzz_setup(seed);
        if (!g.s) return;
        if (g.now >= 0xFFF00000u) {
            static uint8_t f[1514];
            found = 1;
            for (g.step = 0; g.step < STEPS && g.bad == 0; g.step++) fuzz_step(f);
            T_EQ(g.bad, 0);
            T_CHECK(g.now < 0xFFF00000u);                   /* it really crossed 2^32 */
        }
    }
    T_CHECK(found);
}


/* ---- PB-level fuzzer: random MacTCP calls through the glue (mac/glue/mt_pb*.c) ----
 * The same peer model drives a stack owned by the PB dispatcher; the "application" issues
 * random csCodes with random fields on live, released and made-up streams, also from inside
 * its completion routines and ASRs. Properties, in addition to every frame/TCB check above:
 *  - every PB the glue accepted (inProgress) completes exactly once; an immediate result
 *    never also runs the completion routine;
 *  - nothing completes after its stream was released (TCPRelease completes everything before
 *    it returns), and no PB of the stream is still pending then;
 *  - no TCPDataArrival while one of the stream's reads is pending; no PB of the stream is
 *    pending when its TCPTerminate ASR runs (except the call that caused it);
 *  - completed PBs, their buffers and descriptor arrays, and the receive buffer of a released
 *    stream are filled with a canary and must never be written again (nor sent: TCPSend data
 *    is the sequence-number pattern, so a canary byte on the wire fails verify_l4);
 *  - the glue's queues agree with the PBs the application has outstanding; interrupts are
 *    restored and no deferred release is left after every entry. */
#define PS_N 8
#define PR_N 128
#define PR_PB 104
#define PR_BUF 1800
#define PR_DS 12
#define PR_BLK (PR_PB + PR_BUF + PR_DS * 6)
#define PS_BUF 16400
#define CAN2 0x5Au
#define PB_STEPS 12000
#define PB_SEEDS 6

typedef struct {
    uint8_t *blk;              /* PB, then buffer, then the WDS/RDS array (fake memory) */
    int busy, loan, incall, slot, uslot;
    long ncompl;
    StreamPtr h;
    int16_t cs;
} prec;
typedef struct {
    StreamPtr h; uint8_t *rbuf; int live;
    int rx_valid; uint8_t rx_next;   /* peer data is the low byte of its sequence number: reads */
} pstream;                           /* must hand it out contiguously, in stream order */

#define US_N 4
#define US_BUF 4100
typedef struct {
    StreamPtr h; uint8_t *buf; int live;
    uint32_t len; uint16_t port;
    MacPtr32 base;                   /* the receive buffer given to UDPCreate */
    MacPtr32 loan;                   /* the datagram lent by the last completed UDPRead */
} ustream;

static struct {
    MtDrv *d;
    prec r[PR_N];
    pstream ps[PS_N];
    ustream us[US_N];
    long u_read_ok, u_tmo, u_write_ok, u_ret_ok, u_asr, u_rel_pending, u_in, echo_sent, echo_ok,
         echo_tmo, u_lent;
    MacPtr32 compl, asr, uasr;
    long ok_send, term_send, ok_rcv, ok_nocopy, ret_ok, tmo, asr_data, asr_term, asr_closing,
         defer_core, defer_call, imm_err, reenter, open_ok, merged, rx_bytes;
} P;

static TCPiopb *pr_pb(prec *r) { return (TCPiopb *)r->blk; }
static uint8_t *pr_buf(prec *r) { return r->blk + PR_PB; }
static uint8_t *pr_ds(prec *r) { return r->blk + PR_PB + PR_BUF; }

static void canary_check(const uint8_t *p, uint32_t n, const char *what) {
    uint32_t i;
    for (i = 0; i < n; i++) if (p[i] != CAN2) { bad(what); return; }
}

static void pr_free(prec *r) {
    r->busy = r->loan = r->incall = 0;
    memset(r->blk, CAN2, PR_BLK);
}

static prec *pr_get(void) {
    int i, k = (int)rn(PR_N);
    for (i = 0; i < PR_N; i++, k = (k + 1) % PR_N) {
        prec *r = &P.r[k];
        if (r->busy || r->loan || r->incall) continue;
        canary_check(r->blk, PR_BLK, "a completed PB or its buffers were written");
        memset(r->blk, 0, PR_BLK);
        pr_pb(r)->ioCompletion = P.compl;
        pr_pb(r)->ioNamePtr = (MacPtr32)(k + 1);
        r->slot = r->uslot = -1;
        r->h = 0;
        return r;
    }
    return NULL;
}

static MtStream *pb_stream(StreamPtr h) {
    uint8_t i;
    for (i = 0; i < P.d->ntcp; i++)
        if (P.d->tcp[i].used == 1 && P.d->tcp[i].handle == h) return &P.d->tcp[i];
    return NULL;
}

/* ---- UDP and ICMP echo through the PB interface ---- */

static int16_t issue_ex(prec *r, int16_t cs, StreamPtr h, int slot, int uslot, int set_stream);
static void u_lend(int uslot, MacPtr32 key, uint32_t len);
static void pb_udp_invariants(void);

static StreamPtr pick_uhandle(int *uslot) {
    int i, k = (int)rn(US_N);
    *uslot = -1;
    if (rn(12) == 0) return rn(3) == 0 ? 0 : rn(2) ? rnd() : P.us[k].h + 2u;
    for (i = 0; i < US_N; i++, k = (k + 1) % US_N)
        if (P.us[k].live) { *uslot = k; return P.us[k].h; }
    return 0;
}

static void ustream_dead(int slot) {
    int k;
    for (k = 0; k < PR_N; k++)
        if (P.r[k].uslot == slot && P.r[k].busy && !P.r[k].incall) bad("PB still pending after UDPRelease returned");
    P.us[slot].live = 0;
    P.us[slot].h = 0;
    P.us[slot].loan = 0;
    memset(P.us[slot].buf, CAN2, US_BUF);
}

static void act_ucreate(void) {
    int slot = -1, k, k0 = (int)rn(US_N);
    prec *r;
    UDPiopb *pb;
    int16_t res;
    uint32_t len;
    uint16_t port;
    for (k = 0; k < US_N; k++) if (!P.us[(k0 + k) % US_N].live) { slot = (k0 + k) % US_N; break; }
    if (slot < 0 || !(r = pr_get())) return;
    pb = (UDPiopb *)pr_pb(r);
    canary_check(P.us[slot].buf, US_BUF, "receive buffer of a released UDP stream was written");
    len = rn(10) == 0 ? rn(2100) : 2048 + rn(US_BUF - 2048 - 1);
    port = (uint16_t)(rn(3) == 0 ? 0 : rn(2) ? 7 : 1000 + rn(3));
    pb->csParam.create.rcvBuff = rn(20) ? pbf_a(P.us[slot].buf + rn(2)) : 0;
    pb->csParam.create.rcvBuffLen = len;
    pb->csParam.create.notifyProc = rn(8) ? P.uasr : 0;
    pb->csParam.create.localPort = port;
    pb->csParam.create.userDataPtr = (MacPtr32)(slot + 1);
    res = issue_ex(r, UDPCreate, 0, -1, -1, 0);
    if (res == 0) {
        P.us[slot].h = pb->udpStream;
        P.us[slot].live = 1;
        P.us[slot].loan = 0;
        P.us[slot].len = len;
        P.us[slot].base = pb->csParam.create.rcvBuff;
        P.us[slot].port = pb->csParam.create.localPort;
        memset(P.us[slot].buf, 0, US_BUF);
    } else if (len >= 2048 && pb->csParam.create.rcvBuff && port == 0 && res != insufficientResources) {
        bad("valid UDPCreate refused");
    } else if (len < 2048 && res != invalidLength) {
        bad("short UDP buffer not refused");
    }
    if (!r->busy) pr_free(r);
}

static void act_uread(void) {
    int uslot;
    StreamPtr h = pick_uhandle(&uslot);
    prec *r = pr_get();
    UDPiopb *pb;
    int16_t res;
    if (!r) return;
    pb = (UDPiopb *)pr_pb(r);
    pb->csParam.receive.timeOut = (uint16_t)(rn(3) ? 0 : 1 + rn(4));
    res = issue_ex(r, UDPRead, h, -1, uslot, 1);
    if (res == 0 && uslot >= 0) {
        P.u_read_ok++;
        u_lend(uslot, pb->csParam.receive.rcvBuff, pb->csParam.receive.rcvBuffLen);
    }
    if (!r->busy) pr_free(r);
}

static void act_uwrite(void) {
    int uslot, i, ne = (int)rn(4);
    StreamPtr h = pick_uhandle(&uslot);
    prec *r = pr_get();
    UDPiopb *pb;
    wdsEntry e;
    uint32_t pos = rn(2), j;
    int16_t res;
    if (!r) return;
    pb = (UDPiopb *)pr_pb(r);
    memset(pr_buf(r), 0x11, PR_BUF);
    for (i = 0; i < ne; i++) {
        uint32_t len = rn(5) == 0 ? 0 : rn(6) == 0 ? 1400 : rn(300);
        if (pos + len > PR_BUF) len = pos < PR_BUF ? PR_BUF - pos : 0;
        e.length = (uint16_t)len;
        e.ptr = rn(40) ? pbf_a(pr_buf(r) + pos) : 0;
        memcpy(pr_ds(r) + i * 6, &e, 6);
        if (len == 0) break;
        for (j = 0; j < len; j++) pr_buf(r)[pos + j] = (uint8_t)(0x11 + j % 7);
        pos += len;
    }
    if (i == ne) {
        e.length = 0; e.ptr = 0;
        memcpy(pr_ds(r) + i * 6, &e, 6);
    }
    pb->csParam.send.remoteHost = rn(8) ? PEER : (rn(2) ? T_FAR : 0);
    pb->csParam.send.remotePort = (uint16_t)(rn(3) ? 17007 : rn(65536));
    pb->csParam.send.wdsPtr = rn(30) ? pbf_a(pr_ds(r)) : 0;
    pb->csParam.send.checkSum = (uint8_t)rn(2);
    res = issue_ex(r, UDPWrite, h, -1, uslot, 1);
    if (res == 0) P.u_write_ok++;
    if (!r->busy) pr_free(r);
}

static void act_ubfr(void) {
    int uslot, k = (int)rn(US_N), i;
    prec *r;
    UDPiopb *pb;
    int16_t res;
    StreamPtr h;
    for (i = 0; i < US_N; i++, k = (k + 1) % US_N) if (P.us[k].live && P.us[k].loan) break;
    if (i == US_N || !(r = pr_get())) return;
    uslot = k;
    h = P.us[k].h;
    pb = (UDPiopb *)pr_pb(r);
    if (rn(8) == 0) {                                   /* a made-up pointer first */
        pb->csParam.receive.rcvBuff = P.us[k].loan ^ 0x10u;
        res = issue_ex(r, UDPBfrReturn, h, -1, uslot, 1);
        if (res == 0) bad("a made-up datagram pointer was accepted");
        if (!r->busy) pr_free(r);
        if (!(r = pr_get())) return;
        pb = (UDPiopb *)pr_pb(r);
        if (!P.us[k].live || !P.us[k].loan) { pr_free(r); return; }
    }
    {
        MacPtr32 key = P.us[k].loan;
        pb->csParam.receive.rcvBuff = key;
        P.us[k].loan = 0;                       /* returned: the call itself may lend the next datagram */
        res = issue_ex(r, UDPBfrReturn, h, -1, uslot, 1);
        if (res == 0) {
            P.u_ret_ok++;
        } else {
            int glue_live = 0;
            uint8_t q;
            for (q = 0; q < P.d->nudp; q++) glue_live |= P.d->udp[q].used == 1 && P.d->udp[q].handle == h;
            if (glue_live || res != invalidStreamPtr)    /* (else: released by a nested call, not yet known here) */
                bad("returning the lent datagram failed");
            if (P.us[k].live && P.us[k].h == h && !P.us[k].loan) P.us[k].loan = key;
        }
    }
    if (!r->busy) pr_free(r);
}

static void act_urelease(void) {
    int uslot;
    StreamPtr h = pick_uhandle(&uslot);
    prec *r = pr_get();
    int16_t res;
    if (!r) return;
    res = issue_ex(r, UDPRelease, h, -1, uslot, 1);
    if (res == 0 && uslot >= 0) ustream_dead(uslot);
    if (!r->busy) pr_free(r);
}

static void act_uother(void) {
    static const int16_t cs[] = {UDPMaxMTUSize, UDPStatus, UDPMultiCreate, UDPMultiSend, UDPMultiRead,
                                 UDPCreate, UDPRead, UDPWrite, UDPBfrReturn};
    int uslot;
    StreamPtr h = pick_uhandle(&uslot);
    prec *r = pr_get();
    int16_t c = cs[rn(sizeof cs / sizeof cs[0])];
    if (!r) return;
    if (c == UDPRead || c == UDPWrite || c == UDPBfrReturn || c == UDPCreate) h = 0, uslot = -1;   /* the real actions do these */
    issue_ex(r, c, h, -1, uslot, 1);
    if (!r->busy) pr_free(r);
}

static void act_echo(void) {
    prec *r = pr_get();
    IPParamBlock *pb;
    uint32_t len = rn(4) == 0 ? rn(1600) : rn(80), j;
    int16_t res;
    if (!r) return;
    pb = (IPParamBlock *)pr_pb(r);
    if (len > PR_BUF) len = PR_BUF;
    for (j = 0; j < len; j++) pr_buf(r)[j] = (uint8_t)(0x20 + j);
    pb->csParam.IPEchoPB.dest = rn(5) ? PEER : (rn(2) ? GW : rn(2) ? T_FAR : 0);
    pb->csParam.IPEchoPB.data.length = (uint16_t)len;
    pb->csParam.IPEchoPB.data.ptr = len && rn(40) ? pbf_a(pr_buf(r)) : 0;
    pb->csParam.IPEchoPB.timeout = (int16_t)(rn(8) == 0 ? -1 : (int)rn(6));
    pb->csParam.IPEchoPB.icmpCompletion = rn(5) ? P.compl : 0;
    {   /* the PB sits in an exact-size heap block: any read past 54 bytes is an ASan error.
         * An immediate result never runs the completion routine, so r is not busy afterwards. */
        void *h = malloc(sizeof(IPParamBlock));
        memcpy(h, pb, sizeof(IPParamBlock));
        ((IPParamBlock *)h)->csCode = ipctlEchoICMP;
        res = mt_pb_control(P.d, (MtAnyPb *)h);
        free(h);
        if (res == inProgress) bad("ICMPEcho returned inProgress");
        if (res == 0 && pb->csParam.IPEchoPB.icmpCompletion) P.echo_sent++;
    }
    if (!r->busy) pr_free(r);
}

static void act_ipmisc(void) {
    prec *r = pr_get();
    LAPStats *ls = (LAPStats *)(pr_buf(r ? r : &P.r[0]) + 1);
    int16_t res, c = (int16_t)(rn(2) ? ipctlGetAddr : ipctlLAPStats);
    if (!r) return;
    if (c == ipctlLAPStats) ((IPParamBlock *)pr_pb(r))->csParam.LAPStatsPB.lapStatsPtr = rn(10) ? pbf_a(ls) : 0;
    res = issue_ex(r, c, 0, -1, -1, 0);
    if (c == ipctlGetAddr && res != 0) bad("GetMyIPAddr failed");
    if (!r->busy) pr_free(r);
}

/* A datagram from the peer to one of our ports (and, sometimes, a matching or stray echo reply). */
static uint16_t gen_pb_udp(uint8_t *f) {
    uint8_t pl[800];
    uint16_t dl = (uint16_t)(rn(5) == 0 ? 0 : rn(700)), i, dport = 0;
    int k = (int)rn(US_N), n;
    for (n = 0; n < US_N; n++, k = (k + 1) % US_N) if (P.us[k].live) { dport = P.us[k].port; break; }
    if (!dport || rn(10) == 0) dport = (uint16_t)(rn(2) ? 7 : 1 + rn(65535));
    mt_wr16(pl, (uint16_t)(rn(2) ? 17007 : 1 + rn(65535)));
    mt_wr16(pl + 2, dport);
    mt_wr16(pl + 4, (uint16_t)(8 + dl));
    mt_wr16(pl + 6, 0);
    for (i = 0; i < dl; i++) pl[8 + i] = (uint8_t)rnd();
    if (rn(4)) {
        uint32_t sum = (PEER >> 16) + (PEER & 0xFFFFu) + (ME >> 16) + (ME & 0xFFFFu) + 17u + 8u + dl;
        uint16_t c = mt_cksum_fold(mt_cksum_add(sum, pl, 8u + dl));
        mt_wr16(pl + 6, c ? c : 0xFFFFu);
    }
    P.u_in++;
    return mk_ip(f, PEER, ME, 17, (uint16_t)rnd(), 0, pl, (uint16_t)(8 + dl));
}

static uint16_t gen_pb_echo_reply(uint8_t *f) {
    uint8_t pl[100];
    uint16_t i, l = (uint16_t)(8 + rn(60));
    const MtEcho *e = &P.d->echo[rn(MT_PB_ECHOQ)];
    for (i = 0; i < l; i++) pl[i] = (uint8_t)rnd();
    pl[0] = 0;
    pl[1] = 0;
    mt_wr16(pl + 4, e->state ? e->id : (uint16_t)rnd());
    mt_wr16(pl + 6, (uint16_t)(e->seq + (rn(6) == 0)));
    mt_wr16(pl + 2, 0);
    mt_wr16(pl + 2, mt_cksum_fold(mt_cksum_add(0, pl, l)));
    return mk_ip(f, rn(6) ? e->dest : PEER, ME, 1, (uint16_t)rnd(), 0, pl, l);
}

static void pb_action(void);

/* Received bytes continue the stream where the previous read stopped (also across no-copy
 * loans and copies made behind outstanding loans). */
static void rx_check(int slot, const uint8_t *b, uint32_t n) {
    pstream *p;
    uint32_t i;
    if (slot < 0 || !b) return;
    p = &P.ps[slot];
    for (i = 0; i < n; i++) {
        if (p->rx_valid && b[i] != p->rx_next) { bad("received data out of order or wrong"); return; }
        p->rx_next = (uint8_t)(b[i] + 1u);
        p->rx_valid = 1;
    }
    P.rx_bytes += n;
}

static void rx_check_pb(const prec *r, TCPiopb *pb) {
    if (r->cs == TCPRcv) {
        rx_check(r->slot, (const uint8_t *)pbf_p(pb->csParam.receive.rcvBuff),
                 pb->csParam.receive.rcvBuffLen);
    } else if (r->cs == TCPNoCopyRcv) {
        const uint8_t *rds = (const uint8_t *)pbf_p(pb->csParam.receive.rdsPtr);
        int k;
        for (k = 0; rds && k < PR_DS; k++) {
            rdsEntry e;
            memcpy(&e, rds + 6 * k, sizeof e);
            if (!e.length) break;
            rx_check(r->slot, (const uint8_t *)pbf_p(e.ptr), e.length);
        }
    }
}

static void pb_reenter(void) {
    if (g.cb_depth >= 3 || rn(3)) return;
    g.cb_depth++;
    P.reenter++;
    pb_action();
    g.cb_depth--;
}

static void u_lend(int uslot, MacPtr32 key, uint32_t len) {
    ustream *u = &P.us[uslot];
    if (u->loan) bad("a second datagram was lent while one is outstanding");
    if (key < u->base || key + len > u->base + u->len) bad("lent datagram lies outside the receive buffer");
    u->loan = key;
    P.u_lent++;
}

static void fz_compl(MtAnyPb *pb) {
    uint32_t k = pb->hdr.ioNamePtr - 1u;
    prec *r;
    int16_t res = pb->hdr.ioResult;
    if ((uint8_t *)pb >= (uint8_t *)P.d->echo && (uint8_t *)pb < (uint8_t *)(P.d->echo + MT_PB_ECHOQ)) {
        if (pb->hdr.csCode != ipctlEchoICMP || (res != 0 && res != icmpEchoTimeoutErr)) bad("bad echo notify block");
        if (res == 0) P.echo_ok++; else P.echo_tmo++;
        if (P.echo_ok + P.echo_tmo > P.echo_sent) bad("echo notified more often than requested");
        pb_reenter();
        return;
    }
    if (k >= PR_N) { bad("completion for an unknown PB"); return; }
    r = &P.r[k];
    if (!r->busy) { bad("completion of a PB that is not pending (twice, or after an immediate result)"); return; }
    r->busy = 0;
    r->ncompl++;
    if (r->slot >= 0 && P.ps[r->slot].h != r->h) bad("completion after its stream was released");
    if (r->uslot >= 0) {
        if (P.us[r->uslot].h != r->h) bad("UDP completion after its stream was released");
        if (r->cs == UDPRead && res == 0) {
            P.u_read_ok++;
            u_lend(r->uslot, ((UDPiopb *)pb)->csParam.receive.rcvBuff, ((UDPiopb *)pb)->csParam.receive.rcvBuffLen);
        }
        if (r->cs == UDPRead && res == commandTimeout) P.u_tmo++;
        if (r->cs == UDPRead && res == connectionTerminated) P.u_rel_pending++;
    }
    if (r->cs == TCPSend) { if (res == 0) P.ok_send++; else if (res == connectionTerminated) P.term_send++; }
    if (r->cs == TCPRcv && res == 0) P.ok_rcv++;
    if (res == 0) rx_check_pb(r, (TCPiopb *)pb);
    if (res == commandTimeout) P.tmo++;
    if ((r->cs == TCPActiveOpen || r->cs == TCPPassiveOpen) && res == 0) P.open_ok++;
    if (r->cs == TCPNoCopyRcv && res == 0) { r->loan = 1; P.ok_nocopy++; }
    if (!r->incall && !r->loan) pr_free(r);
    pb_reenter();
}

static void fz_uasr(StreamPtr s, uint16_t ev, MacPtr32 user, uint16_t reason, MacPtr32 icmp) {
    int slot = (int)user - 1, k;
    (void)reason; (void)icmp;
    if (slot < 0 || slot >= US_N || P.us[slot].h != s) { bad("UDP ASR for an unknown stream"); return; }
    if (ev == UDPDataArrival) {
        P.u_asr++;
        for (k = 0; k < PR_N; k++)
            if (P.r[k].busy && !P.r[k].incall && P.r[k].uslot == slot && P.r[k].cs == UDPRead)
                bad("UDPDataArrival while a read is pending");
    }
    pb_reenter();
}

static void fz_asr(StreamPtr s, uint16_t ev, MacPtr32 user, uint16_t reason, MacPtr32 icmp) {
    int slot = (int)user - 1, k;
    (void)reason; (void)icmp;
    if (slot < 0 || slot >= PS_N || P.ps[slot].h != s) { bad("ASR for an unknown stream"); return; }
    if (ev == TCPDataArrival) {
        P.asr_data++;
        for (k = 0; k < PR_N; k++)
            if (P.r[k].busy && !P.r[k].incall && P.r[k].slot == slot &&
                (P.r[k].cs == TCPRcv || P.r[k].cs == TCPNoCopyRcv))
                bad("TCPDataArrival while a read is pending");
    } else if (ev == TCPTerminate) {
        P.asr_term++;
        for (k = 0; k < PR_N; k++)
            if (P.r[k].busy && !P.r[k].incall && P.r[k].slot == slot)
                bad("PB still pending at its stream's TCPTerminate ASR");
    } else if (ev == TCPClosing) {
        P.asr_closing++;
    }
    pb_reenter();
}

/* A live stream (mostly), a released one, 0, or garbage. */
static StreamPtr pick_handle(int *slot) {
    int i, k = (int)rn(PS_N);
    *slot = -1;
    if (rn(12) == 0) return rn(2) ? 0 : (rn(2) ? rnd() : P.ps[k].h + 2u);
    for (i = 0; i < PS_N; i++, k = (k + 1) % PS_N)
        if (P.ps[k].live) { *slot = k; return P.ps[k].h; }
    return 0;
}

static void stream_dead(int slot) {
    int k;
    for (k = 0; k < PR_N; k++) {
        prec *r = &P.r[k];
        if (r->slot != slot) continue;
        if (r->busy && !r->incall) bad("PB still pending after TCPRelease returned");
        if (r->loan) pr_free(r);
    }
    P.ps[slot].live = 0;
    P.ps[slot].h = 0;
    memset(P.ps[slot].rbuf, CAN2, PS_BUF);
}

static void free_loans(int slot) {
    int k;
    for (k = 0; k < PR_N; k++) if (P.r[k].slot == slot && P.r[k].loan) pr_free(&P.r[k]);
}

/* Issue a PB: busy before the call (its completion may run inside), then the result. */
static int16_t issue_ex(prec *r, int16_t cs, StreamPtr h, int slot, int uslot, int set_stream) {
    TCPiopb *pb = pr_pb(r);
    long nc = r->ncompl;
    int16_t res;
    pb->csCode = cs;
    if (set_stream) pb->tcpStream = h;
    r->cs = cs;
    r->h = h;
    r->slot = slot;
    r->uslot = uslot;
    r->busy = 1;
    r->incall = 1;
    res = mt_pb_control(P.d, (MtAnyPb *)pb);
    r->incall = 0;
    if (res != inProgress) {
        if (r->ncompl != nc) bad("an immediate result also ran the completion routine");
        if (pb->ioResult != res) bad("ioResult differs from the immediate result");
        r->busy = 0;
        if (res < 0) P.imm_err++;
        if (slot < 0 && cs >= TCPPassiveOpen && cs <= TCPRelease && cs != TCPGlobalInfo &&
            res != invalidStreamPtr && !pb_stream(h))
            bad("bad stream not refused with invalidStreamPtr");
        if (set_stream && slot < 0 && uslot < 0 && (cs == UDPRead || cs == UDPBfrReturn || cs == UDPWrite ||
            cs == UDPRelease || cs == UDPMultiSend || cs == UDPMultiRead) && res != invalidStreamPtr)
            bad("bad UDP stream not refused with invalidStreamPtr");
    }
    return res;
}

static int16_t issue(prec *r, int16_t cs, StreamPtr h, int slot) { return issue_ex(r, cs, h, slot, -1, 1); }

static void act_create(void) {
    int slot = -1, k, k0 = (int)rn(PS_N);
    prec *r;
    TCPiopb *pb;
    int16_t res;
    uint32_t len;
    for (k = 0; k < PS_N; k++) if (!P.ps[(k0 + k) % PS_N].live) { slot = (k0 + k) % PS_N; break; }
    if (slot < 0 || !(r = pr_get())) return;
    pb = pr_pb(r);
    canary_check(P.ps[slot].rbuf, PS_BUF, "receive buffer of a released stream was written");
    len = rn(10) == 0 ? rn(4096) : 4096 + rn(PS_BUF - 4096 - 1);
    pb->csParam.create.rcvBuff = rn(20) ? pbf_a(P.ps[slot].rbuf + rn(2)) : 0;
    pb->csParam.create.rcvBuffLen = len;
    pb->csParam.create.notifyProc = rn(8) ? P.asr : 0;
    pb->csParam.create.userDataPtr = (MacPtr32)(slot + 1);
    res = issue(r, TCPCreate, 0, -1);
    if (res == 0) {
        P.ps[slot].h = pb->tcpStream;
        P.ps[slot].live = 1;
        P.ps[slot].rx_valid = 0;
        memset(P.ps[slot].rbuf, 0, PS_BUF);
    } else if (len >= 4096 && pb->csParam.create.rcvBuff && res != insufficientResources) {
        bad("valid TCPCreate refused");
    }
    if (!r->busy) pr_free(r);
}

static void act_open(int passive) {
    int slot;
    StreamPtr h = pick_handle(&slot);
    prec *r = pr_get();
    TCPiopb *pb;
    if (!r) return;
    pb = pr_pb(r);
    pb->csParam.open.remoteHost = passive ? (rn(4) ? 0 : PEER) : (rn(8) ? PEER : (rn(2) ? T_FAR : 0));
    pb->csParam.open.remotePort = passive ? 0 : (uint16_t)(80 + rn(3));
    pb->csParam.open.localPort = (uint16_t)(passive ? (rn(3) ? 23 : rn(2) ? 21 : 0)
                                            : (rn(4) ? 0 : 1024 + rn(4)));
    pb->csParam.open.validityFlags = (int8_t)(rn(2) ? (timeoutValue | timeoutAction) : rn(2) ? timeoutValue : 0);
    pb->csParam.open.ulpTimeoutValue = (int8_t)rn(30);
    pb->csParam.open.ulpTimeoutAction = (int8_t)rn(2);
    pb->csParam.open.commandTimeoutValue = (int8_t)(rn(3) ? 0 : rn(6));
    if (issue(r, passive ? TCPPassiveOpen : TCPActiveOpen, h, slot) == inProgress && slot >= 0) {
        free_loans(slot);                       /* a new connection forgets old loans */
        P.ps[slot].rx_valid = 0;                /* and its data starts at a new sequence number */
    }
    if (!r->busy) pr_free(r);
}

static void act_send(void) {
    int slot;
    StreamPtr h = pick_handle(&slot);
    prec *r = pr_get();
    TCPiopb *pb;
    MtStream *ms;
    uint32_t seq0 = 0, total = 0, pos = rn(2);
    uint16_t lport = 0;
    int i, ne = rn(12) == 0 ? 0 : 1 + (int)rn(PR_DS - 1);
    wdsEntry e;
    if (!r) return;
    pb = pr_pb(r);
    ms = pb_stream(h);
    if (ms && ms->t) {
        seq0 = ms->t->snd_una + mt_tcp_sq_total(ms->t) + (ms->q_total - ms->sub_total);
        lport = ms->t->lport;
    }
    memset(pr_buf(r), CAN2, PR_BUF);
    for (i = 0; i < ne; i++) {
        uint32_t len = rn(5) == 0 ? 0 : rn(8) == 0 ? 600 : rn(150), j;
        if (len == 0 && rn(2)) len = 1;          /* zero-length entries terminate: keep a few */
        pos += rn(3);
        if (pos + len > PR_BUF) len = pos < PR_BUF ? PR_BUF - pos : 0;
        e.length = (uint16_t)len;
        e.ptr = pbf_a(pr_buf(r) + pos);
        memcpy(pr_ds(r) + i * 6, &e, 6);
        if (len == 0) break;
        for (j = 0; j < len; j++) pr_buf(r)[pos + j] = spat(seq0 + total + j, lport);
        pos += len;
        total += len;
    }
    if (i == ne) {
        e.length = 0; e.ptr = 0;
        memcpy(pr_ds(r) + i * 6, &e, 6);
    }
    pb->csParam.send.wdsPtr = rn(30) ? pbf_a(pr_ds(r)) : 0;
    pb->csParam.send.pushFlag = (uint8_t)rn(2);
    pb->csParam.send.urgentFlag = (uint8_t)(rn(12) == 0);
    pb->csParam.send.validityFlags = (int8_t)(rn(4) == 0 ? timeoutValue : 0);
    pb->csParam.send.ulpTimeoutValue = (int8_t)(5 + rn(60));
    if (ms && ms->req_n >= MT_SENDQ_MAX && total) P.merged++;
    issue(r, TCPSend, h, slot);
    if (!r->busy) pr_free(r);
}

static void act_rcv(int nocopy) {
    int slot;
    StreamPtr h = pick_handle(&slot);
    prec *r = pr_get();
    TCPiopb *pb;
    if (!r) return;
    pb = pr_pb(r);
    pb->csParam.receive.commandTimeoutValue = (int8_t)(rn(3) ? 0 : rn(4));
    if (nocopy) {
        pb->csParam.receive.rdsPtr = rn(20) ? pbf_a(pr_ds(r)) : 0;
        pb->csParam.receive.rdsLength = (uint16_t)(rn(10) == 0 ? rn(2) : 2 + rn(PR_DS - 1));
    } else {
        pb->csParam.receive.rcvBuff = rn(20) ? pbf_a(pr_buf(r) + rn(2)) : 0;
        pb->csParam.receive.rcvBuffLen = (uint16_t)(rn(20) == 0 ? 0 : 1 + rn(PR_BUF - 2));
    }
    if (issue(r, nocopy ? TCPNoCopyRcv : TCPRcv, h, slot) == 0) {
        rx_check_pb(r, pb);
        if (nocopy) {
            r->loan = 1;
            P.ok_nocopy++;
        }
    }
    if (!r->busy && !r->loan) pr_free(r);
}

static void act_return(void) {
    int k = (int)rn(PR_N), i;
    prec *r = NULL;
    int16_t res;
    for (i = 0; i < PR_N; i++, k = (k + 1) % PR_N)
        if (P.r[k].loan && !P.r[k].busy && !P.r[k].incall) { r = &P.r[k]; break; }
    if (!r) return;
    r->incall = 1;
    if (rn(10) == 0) {                          /* a made-up RDS first */
        rdsEntry e;
        memcpy(&e, pr_ds(r), sizeof e);
        e.ptr ^= 0x10u;
        memcpy(pr_ds(r) + 6 * (PR_DS - 2), &e, sizeof e);
        pr_pb(r)->csParam.receive.rdsPtr = pbf_a(pr_ds(r) + 6 * (PR_DS - 2));
        pr_pb(r)->csCode = TCPRcvBfrReturn;
        res = mt_pb_control(P.d, (MtAnyPb *)pr_pb(r));
        if (res == 0) bad("a made-up RDS was accepted");
        pr_pb(r)->csParam.receive.rdsPtr = pbf_a(pr_ds(r));
    }
    pr_pb(r)->csCode = TCPRcvBfrReturn;
    res = mt_pb_control(P.d, (MtAnyPb *)pr_pb(r));
    if (res == 0) P.ret_ok++;
    pr_free(r);
}

static void act_simple(int16_t cs) {
    int slot;
    StreamPtr h = pick_handle(&slot);
    prec *r = pr_get();
    int16_t res;
    if (!r) return;
    if (cs == TCPClose) {
        pr_pb(r)->csParam.close.validityFlags = (int8_t)(rn(3) ? 0 : timeoutValue);
        pr_pb(r)->csParam.close.ulpTimeoutValue = (int8_t)rn(30);
    }
    res = issue(r, cs, h, slot);
    if (cs == TCPRelease && res == 0 && slot >= 0) {
        if (P.d->in_core) P.defer_core++;       /* from a callback of mt_input / mt_tick */
        else if (g.cb_depth) P.defer_call++;    /* from a call-out of mt_pb_control */
        stream_dead(slot);
    }
    if (!r->busy) pr_free(r);
}

static void pb_action(void) {
    switch (rn(30)) {
    case 16: act_ucreate(); break;
    case 17: case 18: act_uread(); break;
    case 19: case 20: act_uwrite(); break;
    case 21: act_ubfr(); break;
    case 22: if (rn(8) == 0) act_urelease(); break;
    case 23: act_uother(); break;
    case 24: case 25: act_echo(); break;
    case 26: act_ipmisc(); break;
    case 0: act_create(); break;
    case 1: act_open(0); break;
    case 2: act_open(1); break;
    case 3: case 4: case 5: case 15: act_send(); break;
    case 6: case 7: act_rcv(0); break;
    case 8: act_rcv(1); break;
    case 9: act_return(); break;
    case 10: if (rn(3) == 0) act_simple(TCPClose); break;
    case 11: if (rn(6) == 0) act_simple(TCPAbort); break;
    case 12: if (rn(10) == 0) act_simple(TCPRelease); break;
    case 13: act_simple(rn(3) ? TCPStatus : rn(2) ? TCPGlobalInfo : TCPExtendedStat); break;
    case 14: {                                  /* any other csCode with a zeroed PB */
        int16_t cs = (int16_t)rn(61);
        if (cs != TCPRelease && cs != TCPSend && cs != TCPCreate && cs != ipctlGetAddr &&
            cs != ipctlEchoICMP && cs != ipctlLAPStats && cs != UDPCreate && cs != UDPMultiCreate)
            act_simple(cs);                     /* (those take pointers in the stream's place: own actions) */
        break;
    }
    default: break;
    }
}

static void pb_invariants(void) {
    int i, k;
    if (pbf_irq != 0) bad("interrupt mask not restored");
    if (P.d->depth || P.d->in_core) bad("glue depth not back to 0");
    if (P.d->rel_mask) bad("deferred release left over");
    if (pbf_bad) { bad("misuse seen by the fake OS (pointer, routine, irq nesting)"); pbf_bad = 0; }
    pb_udp_invariants();
    for (i = 0; i < PS_N; i++) {
        MtStream *s;
        int nsend = 0, nrcv = 0, nopen = 0, nclose = 0;
        uint32_t lent = 0;
        uint8_t j;
        if (!P.ps[i].live) {
            for (k = 0; k < PR_N; k++)
                if (P.r[k].slot == i && P.r[k].busy && !P.r[k].incall) bad("PB pending on a released stream");
            continue;
        }
        s = pb_stream(P.ps[i].h);
        if (!s) { bad("live stream unknown to the glue"); continue; }
        for (k = 0; k < PR_N; k++) {
            const prec *r = &P.r[k];
            if (r->slot != i || !r->busy || r->incall) continue;
            if (r->cs == TCPSend) nsend++;
            else if (r->cs == TCPRcv || r->cs == TCPNoCopyRcv) nrcv++;
            else if (r->cs == TCPActiveOpen || r->cs == TCPPassiveOpen) nopen++;
            else if (r->cs == TCPClose) nclose++;
        }
        if (nsend != s->sq_n) bad("pending TCPSend PBs differ from the glue's queue");
        if (nrcv != s->rq_n) bad("pending reads differ from the glue's queue");
        if (nopen != (s->open_pb != NULL)) bad("pending open differs");
        if (nclose != (s->close_pb != NULL)) bad("pending close differs");
        if (s->sub_i > s->sq_n || s->req_n > MT_SENDQ_MAX || s->loan_n > MT_PB_LOANS) bad("glue queue bounds");
        if ((int32_t)(s->sub_total - s->acked) < 0 || (int32_t)(s->q_total - s->sub_total) < 0)
            bad("glue send offsets out of order");
        for (j = 0; j < s->loan_n; j++) lent += s->loan[j].len;
        if (lent != s->lent) bad("loan bytes disagree");
        if (s->t) {
            mt_tcp_info in;
            mt_tcp_get_info(s->t, &in);
            if (s->lent > in.rcv_avail) bad("more lent than the ring holds");
            if (in.state >= MT_TCP_ESTABLISHED && in.state != MT_TCP_TIME_WAIT &&
                in.unacked + in.send_queued > s->sub_total - s->acked)
                bad("core holds more bytes than the glue submitted");
        }
    }
}

static void pb_udp_invariants(void) {
    int i, k, nwait = 0;
    uint8_t j;
    for (i = 0; i < US_N; i++) {
        const MtUdpStream *s = NULL;
        int nrd = 0;
        if (!P.us[i].live) {
            for (k = 0; k < PR_N; k++)
                if (P.r[k].uslot == i && P.r[k].busy && !P.r[k].incall) bad("PB pending on a released UDP stream");
            continue;
        }
        for (j = 0; j < P.d->nudp; j++)
            if (P.d->udp[j].used == 1 && P.d->udp[j].handle == P.us[i].h) s = &P.d->udp[j];
        if (!s) { bad("live UDP stream unknown to the glue"); continue; }
        for (k = 0; k < PR_N; k++)
            if (P.r[k].uslot == i && P.r[k].busy && !P.r[k].incall && P.r[k].cs == UDPRead) nrd++;
        if (nrd != s->rq_n) bad("pending UDPReads differ from the glue's queue");
        if (s->loan != P.us[i].loan) bad("lent datagram differs from the glue's");
        if (s->u && mt_udp_port(s->u) != P.us[i].port) bad("UDP port changed");
        if (s->rq_n && !s->loan && s->u && mt_udp_queued(s->u)) bad("read waits although a datagram is queued and none lent");
    }
    for (j = 0; j < MT_PB_ECHOQ; j++) nwait += P.d->echo[j].state != 0 && P.d->echo[j].blk.ioCompletion != 0;
    if ((long)nwait != P.echo_sent - P.echo_ok - P.echo_tmo) bad("waiting echoes differ from requests minus notifications");
}

static void pb_full_canary_check(void) {
    int k;
    for (k = 0; k < PR_N; k++)
        if (!P.r[k].busy && !P.r[k].loan && !P.r[k].incall)
            canary_check(P.r[k].blk, PR_BLK, "a completed PB or its buffers were written");
    for (k = 0; k < PS_N; k++)
        if (!P.ps[k].live) canary_check(P.ps[k].rbuf, PS_BUF, "receive buffer of a released stream was written");
    for (k = 0; k < US_N; k++)
        if (!P.us[k].live) canary_check(P.us[k].buf, US_BUF, "receive buffer of a released UDP stream was written");
}

static void pb_setup(uint32_t seed) {
    mt_limits lim;
    mt_link link;
    mt_ifconfig c;
    uint32_t n;
    int k;
    memset(&g, 0, sizeof g);
    memset(&P, 0, sizeof P);
    memset(g_rq, 0, sizeof g_rq);               /* the core fuzzer's records: none here */
    memset(g_serial, 0, sizeof g_serial);
    memset(g_next_done, 0, sizeof g_next_done);
    g.seed = seed;
    g.rng = seed * 2654435761u + 7u;
    pbf_reset();
    P.compl = pbf_compl(fz_compl);
    P.asr = pbf_asr(fz_asr);
    P.uasr = pbf_asr(fz_uasr);
    link.ctx = NULL;
    link.tx = link_tx;
    mt_limits_default(&lim);
    n = mt_pb_size(&lim);
    P.d = mt_pb_init(pbf_alloc(n), n, &lim, &link, rnd());
    T_CHECK(P.d != NULL);
    if (!P.d) return;
    g.s = P.d->s;
    memset(&c, 0, sizeof c);
    c.addr = ME; c.mask = 0xFFFFFF00u; c.gateway = GW; c.dns = PEER;
    memcpy(c.mac, T_ME_MAC, 6);
    c.mtu = rn(3) ? 1500 : 576;
    mt_pb_set_config(P.d, &c);
    for (k = 0; k < PR_N; k++) { P.r[k].blk = (uint8_t *)pbf_alloc(PR_BLK); pr_free(&P.r[k]); }
    for (k = 0; k < PS_N; k++) { P.ps[k].rbuf = (uint8_t *)pbf_alloc(PS_BUF); memset(P.ps[k].rbuf, CAN2, PS_BUF); }
    for (k = 0; k < US_N; k++) { P.us[k].buf = (uint8_t *)pbf_alloc(US_BUF); memset(P.us[k].buf, CAN2, US_BUF); }
    g.now = rn(2) ? 0xFFF00000u + rnd() % 0x100000u : rnd();
    pbf_now = g.now;
}

static void pb_step(uint8_t *f) {
    uint32_t d, dt;
    int k, live = 0;
    pbf_now = g.now;
    for (k = 0; k < PS_N; k++) live += P.ps[k].live;
    if (live < 3) act_create();
    if (rn(3)) {
        uint16_t n = gen_frame(f);
        if (rn(20) == 0 && n > 14) mutate(f, n);
        mt_pb_input(P.d, f, n);
    }
    if (rn(8) == 0 && g.lastn) {
        k = (int)rn(g.lastn < 8 ? (uint32_t)g.lastn : 8u);
        mt_pb_input(P.d, g.lastout[k], g.lastlen[k]);
    }
    if (rn(3) == 0) mt_pb_input(P.d, f, gen_pb_udp(f));
    if (rn(8) == 0) mt_pb_input(P.d, f, gen_pb_echo_reply(f));
    for (k = (int)rn(3); k > 0; k--) pb_action();
    check_invariants();
    pb_invariants();
    if ((g.step & 63) == 0) pb_full_canary_check();
    for (k = 0; k < 6 && mt_pb_next_deadline_ms(P.d) == 0; k++) mt_pb_tick(P.d);
    if (k == 6) bad("expired deadline survives mt_pb_tick");
    d = mt_pb_next_deadline_ms(P.d);
    dt = rn(10) == 0 ? rn(5000) : rn(150);
    if (d != MT_NO_DEADLINE && d < dt && rn(2)) {
        g.now += d;
        pbf_now = g.now;
        mt_pb_tick(P.d);
        dt -= d;
    }
    g.now += dt;
    pbf_now = g.now;
    if (mt_pb_next_deadline_ms(P.d) == 0) mt_pb_tick(P.d);
    pb_invariants();
}

static void test_fuzz_pb(void) {
    static uint8_t f[1514];
    uint32_t seed;
    long sok = 0, sterm = 0, rok = 0, nc = 0, ret = 0, tmo = 0, ad = 0, at = 0, ac = 0, drc = 0,
         drp = 0, ie = 0, re = 0, op = 0, mg = 0, early = 0, rx = 0;
    long uro = 0, utm = 0, uwr = 0, urt = 0, uas = 0, urp = 0, uin = 0, eok = 0, eto = 0, ese = 0;
    /* FZ_SEED=n runs only seed n (replay a failure); FZ_SEEDS=n runs seeds 1..n */
    uint32_t first = getenv("FZ_SEED") ? (uint32_t)atoi(getenv("FZ_SEED")) : 1;
    uint32_t last = getenv("FZ_SEED") ? first : getenv("FZ_SEEDS") ? (uint32_t)atoi(getenv("FZ_SEEDS")) : PB_SEEDS;
    long steps = getenv("FZ_STEPS") ? atol(getenv("FZ_STEPS")) : PB_STEPS;   /* FZ_STEPS=n steps per seed */
    for (seed = first; seed <= last; seed++) {
        pb_setup(seed);
        if (!P.d) return;
        for (g.step = 0; g.step < steps && g.bad == 0; g.step++) pb_step(f);
        if (g.bad == 0) pb_full_canary_check();
        T_EQ(g.bad, 0);
        if (g.bad) fprintf(stderr, "pb fuzz seed %u failed at step %ld\n", (unsigned)seed, g.step);
        sok += P.ok_send; sterm += P.term_send; rok += P.ok_rcv; nc += P.ok_nocopy; ret += P.ret_ok;
        tmo += P.tmo; ad += P.asr_data; at += P.asr_term; ac += P.asr_closing; drc += P.defer_core;
        drp += P.defer_call; ie += P.imm_err; re += P.reenter; op += P.open_ok; mg += P.merged;
        early += (long)P.d->early_done; rx += P.rx_bytes;
        uro += P.u_read_ok; utm += P.u_tmo; uwr += P.u_write_ok; urt += P.u_ret_ok; uas += P.u_asr;
        urp += P.u_rel_pending; uin += P.u_in; eok += P.echo_ok; eto += P.echo_tmo; ese += P.echo_sent;
    }
    if (getenv("FUZZ_VERBOSE"))
        fprintf(stderr, "pb: open %ld send ok %ld term %ld rcv ok %ld nocopy %ld returned %ld timeouts %ld "
                "asr data %ld term %ld closing %ld deferred releases in core %ld in call-outs %ld "
                "immediate errors %ld re-entries %ld sends past the core queue %ld early in merged "
                "%ld bytes received %ld\n",
                op, sok, sterm, rok, nc, ret, tmo, ad, at, ac, drc, drp, ie, re, mg, early, rx);
    if (getenv("FUZZ_VERBOSE"))
        fprintf(stderr, "pb udp/icmp: datagrams in %ld reads ok %ld timeouts %ld writes %ld returned %ld "
                "data asr %ld reads failed by release %ld; echoes sent %ld replied %ld timed out %ld\n",
                uin, uro, utm, uwr, urt, uas, urp, ese, eok, eto);
    T_CHECK(uro > 0); T_CHECK(utm > 0); T_CHECK(uwr > 0); T_CHECK(urt > 0); T_CHECK(uas > 0);
    T_CHECK(urp > 0); T_CHECK(eok > 0); T_CHECK(eto > 0);
    /* coverage: the run must reach the interesting paths */
    T_CHECK(op > 0); T_CHECK(sok > 0); T_CHECK(sterm > 0); T_CHECK(rok > 0); T_CHECK(nc > 0);
    T_CHECK(ret > 0); T_CHECK(tmo > 0); T_CHECK(ad > 0); T_CHECK(at > 0); T_CHECK(ac > 0);
    T_CHECK(drc > 0); T_CHECK(drp > 0); T_CHECK(ie > 0); T_CHECK(re > 0); T_CHECK(mg > 0);
    T_CHECK(early > 0); T_CHECK(rx > 0);
}

/* The fuzzer's frame damage must always be detectable by TCP: 400000 mutations of one segment
 * (fixed seed) never leave a frame whose TCP-covered bytes changed yet whose TCP checksum still
 * verifies. Without the guard in mutate() some do (flips whose ones-complement changes cancel,
 * e.g. the same value added to one 16-bit word and subtracted from another), and one such frame
 * delivered pattern-breaking bytes to rx_check (PB fuzzer, FZ_SEED=15 FZ_STEPS=30000, step
 * 23371). */
static void test_fuzz_mutate_always_detectable(void) {
    uint8_t base[200], f[200], d[100];
    uint16_t n, i;
    long k, changed = 0, undetected = 0;
    memset(&g, 0, sizeof g);
    g.rng = 12345u;
    for (i = 0; i < sizeof d; i++) d[i] = (uint8_t)(1000u + i);
    n = mk_tcp(base, PEER, 1026, ME, 23, 1000, 7, TF_ACK, 8192, NULL, 0, d, sizeof d);
    T_CHECK(tcp_cksum_verifies(base, n) != 0);
    for (k = 0; k < 400000; k++) {
        memcpy(f, base, n);
        mutate(f, n);
        if (!tcp_visible_change(base, f, n)) continue;
        changed++;
        if (tcp_cksum_verifies(f, n)) undetected++;
    }
    T_CHECK(changed > 300000);
    T_EQ(undetected, 0);
}

T_MAIN(test_fuzz_seeds, test_fuzz_clock_wrap, test_fuzz_pb, test_fuzz_mutate_always_detectable)
