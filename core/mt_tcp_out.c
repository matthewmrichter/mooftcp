#include <string.h>
#include "mt_int.h"

/* TCP segment output. Every segment is built in place at mt_ip_payload(s) and sent before
 * the caller returns; payload bytes are copied out of the send ring and checksummed in one
 * pass (mt_cksum_copy). All wire fields go through mt_rd/mt_wr (txbuf has no alignment
 * guarantee for the 16/32-bit fields). */

#define TCP_HDR 20u
#define TCP_HDR_SYN 24u            /* header + MSS option */

uint16_t mt_tcp_local_mss(const mt_stack *s) {
    uint16_t mtu = s->cfg.mtu;
    if (mtu > MT_TXBUF_SIZE - MT_ETH_HDR) mtu = (uint16_t)(MT_TXBUF_SIZE - MT_ETH_HDR);
    return mtu > 104u ? (uint16_t)(mtu - 40u) : 64u;   /* floor 64: output can never spin */
}

/* Header at the IP payload area; returns its length. */
static uint16_t build_hdr(mt_stack *s, uint16_t lport, uint16_t rport, uint32_t seq, uint32_t ack,
                          uint8_t flags, uint16_t win, uint16_t up) {
    uint8_t *h = mt_ip_payload(s);
    uint16_t hl = (flags & MT_TH_SYN) ? TCP_HDR_SYN : TCP_HDR;
    mt_wr16(h, lport);
    mt_wr16(h + 2, rport);
    mt_wr32(h + 4, seq);
    mt_wr32(h + 8, (flags & MT_TH_ACK) ? ack : 0u);
    h[12] = (uint8_t)(hl << 2);            /* data offset in words, high nibble */
    h[13] = flags;
    mt_wr16(h + 14, win);
    mt_wr16(h + 16, 0);
    mt_wr16(h + 18, up);                   /* urgent pointer */
    if (hl == TCP_HDR_SYN) {
        h[20] = 2;
        h[21] = 4;
        mt_wr16(h + 22, mt_tcp_local_mss(s));
    }
    return hl;
}

/* Checksum header + `psum` (payload sum already folded in by the caller) and send. */
static void finish(mt_stack *s, mt_ip4 rip, uint16_t hl, uint16_t plen, uint32_t psum,
                   uint32_t now) {
    uint8_t *h = mt_ip_payload(s);
    uint16_t tl = (uint16_t)(hl + plen);
    uint32_t sum = mt_tcp_pseudo(s->cfg.addr, rip, tl) + psum;
    sum = mt_cksum_add(sum, h, hl);          /* hl is even: chaining is exact */
    mt_wr16(h + 16, mt_cksum_fold(sum));
    s->stats.tcp_out++;
    mt_ip_output(s, rip, 6, tl, now);
}

void mt_tcp_send_raw(mt_stack *s, mt_ip4 rip, uint16_t lport, uint16_t rport, uint32_t seq,
                     uint32_t ack, uint8_t flags, uint32_t now) {
    uint16_t hl;
    if (s->cfg.addr == 0) return;
    if (flags & MT_TH_RST) s->stats.tcp_resets_out++;
    hl = build_hdr(s, lport, rport, seq, ack, flags, 0, 0);
    finish(s, rip, hl, 0, 0, now);
}

/* Ones-complement sum of the two ring runs holding n bytes at offset off, copied to dst.
 * The second run starts at an odd stream offset when the first run is odd, so its sum is
 * byte-swapped before being added (mt_cksum_add's chaining rule). */
static uint32_t copy_ring(const mt_ring *r, uint32_t off, uint8_t *dst, uint32_t n) {
    const uint8_t *p;
    uint32_t c1 = mt_ring_peek(r, off, &p), sum, s2;
    if (c1 > n) c1 = n;
    sum = mt_cksum_copy(0, dst, p, c1);
    if (n > c1) {
        mt_ring_peek(r, off + c1, &p);
        s2 = mt_cksum_copy(0, dst + c1, p, n - c1);
        sum += (c1 & 1u) ? mt_cksum_odd(s2) : s2;
    }
    return sum;
}

uint32_t mt_tcp_rcv_window(mt_tcp *t, int commit) {
    uint32_t free_ = mt_ring_free(&t->rcvq), cur = t->rcv_adv - t->rcv_nxt, thr, win;
    if ((cur & 0x80000000u) != 0u) cur = 0;          /* edge is behind rcv_nxt: window used up */
    if (cur > free_) cur = free_;
    thr = t->rcvq.size / 2u;
    if (t->mss < thr) thr = t->mss;
    win = (free_ - cur >= thr) ? free_ : cur;        /* withhold growth smaller than thr */
    if (win > 65535u) win = 65535u;
    if (commit) t->rcv_adv = t->rcv_nxt + win;
    return win;
}

void mt_tcp_xmit(mt_tcp *t, uint32_t seq, uint8_t flags, uint32_t off, uint32_t n, uint32_t now) {
    mt_stack *s = t->st;
    uint32_t psum = 0;
    uint16_t hl, up = 0;
    if (s->cfg.addr == 0) return;
    if (flags & MT_TH_RST) s->stats.tcp_resets_out++;
    if ((t->flags2 & MT_T2_SNDURG) && mt_seq_lt(seq, t->snd_up) && t->snd_up - seq <= 65535u) {
        flags |= MT_TH_URG;                           /* urgent pointer: offset just past the data */
        up = (uint16_t)(t->snd_up - seq);
    }
    hl = build_hdr(s, t->lport, t->rport, seq, t->rcv_nxt, flags,
                   (uint16_t)mt_tcp_rcv_window(t, 1), up);
    if (n) psum = copy_ring(&t->sndq, off, mt_ip_payload(s) + hl, n);
    if (flags & MT_TH_ACK) {
        t->flags &= (uint8_t)~MT_TF_ACKNOW;
        t->flags2 &= (uint8_t)~MT_T2_ACKHALF;
        t->tmask &= (uint8_t)~(1u << MT_TT_DELACK);
    }
    finish(s, t->rip, hl, (uint16_t)n, psum, now);
}

static void arm_rto(mt_tcp *t, uint32_t now) {
    if (!(t->tmask & (1u << MT_TT_RTO))) {
        if (!(t->tmask & (1u << MT_TT_PERSIST))) t->ulp_dl = now + t->ulp_ms;
        t->tmask |= (uint8_t)(1u << MT_TT_RTO);
        t->tmr[MT_TT_RTO] = now + t->rto;
    }
}

/* Zero peer window with nothing in flight: probe at the (backing-off) RTO instead of
 * retransmitting. Replaces the RTO timer; the ULP deadline carries over. */
static void arm_persist(mt_tcp *t, uint32_t now) {
    if (!(t->tmask & (1u << MT_TT_PERSIST))) {
        if (!(t->tmask & (1u << MT_TT_RTO))) t->ulp_dl = now + t->ulp_ms;
        t->tmask = (uint8_t)((t->tmask & ~(1u << MT_TT_RTO)) | (1u << MT_TT_PERSIST));
        t->tmr[MT_TT_PERSIST] = now + t->rto;
    }
}

/* One-byte window probe: the next unsent byte, at snd_nxt. snd_nxt is not advanced (the byte
 * stays "unsent"); snd_max covers it so an ACK that accepts the byte is valid. */
void mt_tcp_probe(mt_tcp *t, uint32_t now) {
    uint32_t sent = t->snd_nxt - t->snd_una;
    if (sent >= t->sndq.used) return;
    mt_tcp_xmit(t, t->snd_nxt, MT_TH_ACK, sent, 1, now);
    if (mt_seq_le(t->snd_max, t->snd_nxt)) t->snd_max = t->snd_nxt + 1u;
}

/* Recovery model (go-back-N): after an RTO or an ARP resolution the caller pulls snd_nxt back
 * to snd_una; the loop below then resends from the send ring, bounded by min(snd_wnd, cwnd),
 * so a burst of lost segments is refilled ack-clocked instead of one hole per RTO. Anything
 * with seq < snd_max is a retransmission: it bypasses Nagle and is never timed (Karn). */
void mt_tcp_output(mt_tcp *t, uint32_t now) {
    uint32_t sent, unsent, inflight, wnd, n, lim;
    int any = 0, idle;
    t->tnow = now;
    switch (t->state) {
    case MT_TCP_SYN_SENT:
    case MT_TCP_SYN_RCVD:
        if (t->snd_nxt == t->iss) {                       /* SYN (or SYN-ACK) not yet sent */
            mt_tcp_xmit(t, t->iss, (uint8_t)(t->state == MT_TCP_SYN_RCVD
                                               ? MT_TH_SYN | MT_TH_ACK : MT_TH_SYN), 0, 0, now);
            t->snd_nxt = t->snd_max = t->iss + 1u;
            arm_rto(t, now);
        } else if (t->flags & MT_TF_ACKNOW) {
            mt_tcp_xmit(t, t->snd_nxt, MT_TH_ACK, 0, 0, now);
        }
        return;
    case MT_TCP_CLOSED:
    case MT_TCP_LISTEN:
        return;
    case MT_TCP_TIME_WAIT:
        if (t->flags & MT_TF_ACKNOW) mt_tcp_xmit(t, t->snd_nxt, MT_TH_ACK, 0, 0, now);
        return;
    default:
        break;
    }
    inflight = t->snd_nxt - t->snd_una;
    idle = t->snd_max == t->snd_una;                      /* nothing unacked at all (Nagle) */
    sent = inflight > t->sndq.used ? t->sndq.used : inflight;   /* data bytes; a FIN is not in the ring */
    unsent = t->sndq.used - sent;
    lim = t->snd_wnd < t->cwnd ? t->snd_wnd : t->cwnd;
    wnd = lim > inflight ? lim - inflight : 0u;
    while (unsent && wnd) {
        uint8_t fl = MT_TH_ACK;
        int ok, retx = mt_seq_lt(t->snd_nxt, t->snd_max);
        n = unsent < wnd ? unsent : wnd;
        if (n > t->mss) n = t->mss;
        if (n == 0) break;
        /* Nagle / sender SWS: a sub-MSS segment goes out when nothing was unacked, or when it
         * ends the queue and the app pushed (or closed). Otherwise (data in flight, no push) it
         * waits for the ACK; `idle` is sampled once, so a bulk write from idle goes out whole. */
        ok = idle || retx || n >= t->mss ||
             (n == unsent && ((t->flags2 & MT_T2_PUSHQ) || (t->flags & MT_TF_FINQ)));
        if (!ok) break;
        if (n == unsent) {
            fl |= MT_TH_PSH;
            if (t->flags & MT_TF_FINQ) fl |= MT_TH_FIN;
        }
        mt_tcp_xmit(t, t->snd_nxt, fl, sent, n, now);
        if (retx) {
            t->st->stats.tcp_rexmt++;
            t->flags2 &= (uint8_t)~MT_T2_RTTING;
        } else if (!(t->flags2 & MT_T2_RTTING)) {
            t->flags2 |= MT_T2_RTTING;                    /* time this segment (one at a time) */
            t->rtt_seq = t->snd_nxt + n + ((fl & MT_TH_FIN) ? 1u : 0u);
            t->rtt_t0 = now;
        }
        t->snd_nxt += n;
        if (fl & MT_TH_FIN) {
            t->snd_nxt++;
            t->flags |= MT_TF_FINSENT;
        }
        if (mt_seq_gt(t->snd_nxt, t->snd_max)) t->snd_max = t->snd_nxt;
        sent += n;
        unsent -= n;
        wnd -= n;
        any = 1;
        arm_rto(t, now);
    }
    if (!unsent) t->flags2 &= (uint8_t)~MT_T2_PUSHQ;
    if (!unsent && (t->flags & MT_TF_FINQ) && t->snd_nxt - t->snd_una <= t->sndq.used &&
        !((t->flags & MT_TF_FINSENT) && t->snd_una == t->snd_max)) {
        int retx = mt_seq_lt(t->snd_nxt, t->snd_max);     /* FIN not in snd_nxt nor acked: (re)send it */
        mt_tcp_xmit(t, t->snd_nxt, MT_TH_ACK | MT_TH_FIN, 0, 0, now);
        if (retx) {
            t->st->stats.tcp_rexmt++;
            t->flags2 &= (uint8_t)~MT_T2_RTTING;
        }
        t->snd_nxt++;
        t->flags |= MT_TF_FINSENT;
        if (mt_seq_gt(t->snd_nxt, t->snd_max)) t->snd_max = t->snd_nxt;
        any = 1;
        arm_rto(t, now);
    }
    if (t->snd_wnd == 0 && unsent && t->snd_nxt == t->snd_una) {
        arm_persist(t, now);
    } else if (t->tmask & (1u << MT_TT_PERSIST)) {
        t->tmask &= (uint8_t)~(1u << MT_TT_PERSIST);      /* window reopened: leave persist */
        t->rto = mt_tcp_rto_base(t);
    }
    if (t->snd_max != t->snd_una && !(t->tmask & ((1u << MT_TT_RTO) | (1u << MT_TT_PERSIST))))
        arm_rto(t, now);                                  /* anything outstanding has a timer */
    if (!any && (t->flags & MT_TF_ACKNOW)) mt_tcp_xmit(t, t->snd_nxt, MT_TH_ACK, 0, 0, now);
}

/* Retransmit the first unacked segment (SYN, SYN-ACK, data from the ring, or FIN). */
void mt_tcp_rexmit(mt_tcp *t, uint32_t now) {
    uint32_t out, data;
    t->tnow = now;
    switch (t->state) {
    case MT_TCP_SYN_SENT:
        mt_tcp_xmit(t, t->iss, MT_TH_SYN, 0, 0, now);
        break;
    case MT_TCP_SYN_RCVD:
        mt_tcp_xmit(t, t->iss, MT_TH_SYN | MT_TH_ACK, 0, 0, now);
        break;
    case MT_TCP_ESTABLISHED: case MT_TCP_FIN_WAIT_1: case MT_TCP_FIN_WAIT_2:
    case MT_TCP_CLOSE_WAIT: case MT_TCP_CLOSING: case MT_TCP_LAST_ACK:
        out = t->snd_max - t->snd_una;
        data = out - ((t->flags & MT_TF_FINSENT) ? 1u : 0u);
        if (data) {
            uint32_t n = data < t->mss ? data : t->mss;
            uint8_t fl = MT_TH_ACK;
            if (n == data && (t->flags & MT_TF_FINSENT)) fl |= MT_TH_FIN;
            mt_tcp_xmit(t, t->snd_una, fl, 0, n, now);
        } else if (t->flags & MT_TF_FINSENT) {
            mt_tcp_xmit(t, t->snd_una, MT_TH_ACK | MT_TH_FIN, 0, 0, now);
        } else {
            return;
        }
        break;
    default:
        return;
    }
    t->flags2 &= (uint8_t)~MT_T2_RTTING;                  /* Karn: nothing in flight is timed now */
    t->st->stats.tcp_rexmt++;
}
