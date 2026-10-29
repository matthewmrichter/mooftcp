#include "t.h"
#include "net.h"
#include "mt_int.h"
#include "mt_util.h"

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
 *    leaves no expired deadline behind. */

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

static int link_tx(void *ctx, const uint8_t *f, uint16_t len) {
    (void)ctx;
    g.ntx++;
    if (g.checking_early) bad("transmit from a tick that was not due");
    verify_out(f, len);
    memcpy(g.lastout[g.lastn & 7], f, len);
    g.lastlen[g.lastn & 7] = len;
    g.lastn++;
    return rn(50) == 0;                                     /* 2% link errors */
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
    }
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

/* Flip 1..4 bytes of the IP packet; half the time repair the IP header checksum so the damage
 * reaches the upper layers. */
static void mutate(uint8_t *f, uint16_t n) {
    uint32_t k, m = 1 + rn(4), hl;
    for (k = 0; k < m; k++) f[14 + rn((uint32_t)n - 14)] ^= (uint8_t)(1 + rn(255));
    if (rn(2)) {
        hl = (uint32_t)(f[14] & 15) << 2;
        if (hl > (uint32_t)n - 14) hl = 20;
        mt_wr16(f + 24, 0);
        mt_wr16(f + 24, mt_cksum_fold(mt_cksum_add(0, f + 14, hl)));
    }
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
    long reopen = 0;
    for (seed = 1; seed <= NSEEDS; seed++) {
        fuzz_seed(seed);
        est += g.ev[MT_TCP_EV_ESTABLISHED]; data += g.ev[MT_TCP_EV_DATA];
        term += g.ev[MT_TCP_EV_TERMINATE]; tx += g.ntx;
        dok += g.done_ok; dab += g.done_abort; busy += g.busy; two += g.two_queued;
        reopen += g.reopen_in_done;
        inval += g.inval;
    }
    if (getenv("FUZZ_VERBOSE"))
        fprintf(stderr, "est %ld data %ld term %ld tx %ld done ok %ld abort %ld busy %ld two %ld inval %ld"
                " reopen %ld\n", est, data, term, tx, dok, dab, busy, two, inval, reopen);
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

T_MAIN(test_fuzz_seeds, test_fuzz_clock_wrap)
