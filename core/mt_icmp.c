#include <string.h>
#include "mt_int.h"

/* ICMP (RFC 792): echo request/reply and error dispatch to TCP/UDP.
 *
 * Policy: messages from zero, broadcast, multicast/class E or the directed-broadcast
 * address are dropped, and only messages addressed to our unicast address (or 127/8) are
 * processed, so we never answer or send to such peers. A source that is one of our own
 * addresses can only come from the loopback FIFO (mt_ip_input drops it from the wire), so
 * a ping to ourselves is answered; the reply (type 0) never provokes another message, and no
 * error is ever originated toward our own address (mt_icmp_send_unreach). */

#define ICMP_ECHO_REPLY 0
#define ICMP_UNREACH    3
#define ICMP_SQUENCH    4
#define ICMP_ECHO       8
#define ICMP_TIMEEXC    11
#define ICMP_PARAMPROB  12

#define ICMP_HDR 8u
#define ICMP_ERR_GAP_MS 100u
/* Largest ICMP message that fits one frame: mt_ip_payload() capacity. */
#define ICMP_ONE_FRAME (MT_TXBUF_SIZE - MT_ETH_HDR - MT_IP_HDR)

static int unicast_src(const mt_stack *s, mt_ip4 a) {
    return a != 0 && a < 0xE0000000u && a != (s->cfg.addr | ~s->cfg.mask);
}

/* icmp_out counts messages handed to the link or the loopback FIFO: call after a successful
 * mt_ip_output* (which also reports MT_OK when the link or the FIFO refused the frame); err0 is
 * tx_fails() from before it. */
static uint32_t tx_fails(const mt_stack *s) { return s->stats.eth_tx_err + s->stats.ip_lo_drops; }
static void icmp_counted(mt_stack *s, uint32_t err0) {
    if (tx_fails(s) == err0) s->stats.icmp_out++;
}

void mt_set_echo_cb(mt_stack *s, mt_echo_cb cb, void *user) {
    s->echo_cb = cb;
    s->echo_user = user;
}

/* Reply to an echo request: verify, then rewrite type 8 -> 0 and patch the checksum
 * incrementally (RFC 1624: HC' = ~(~HC + ~m + m'), m = 0x0800, m' = 0). */
static void icmp_echo_reply(mt_stack *s, mt_ip4 src, const uint8_t *pl, uint16_t len,
                            uint32_t now) {
    uint8_t *out;
    uint32_t t, err0;
    if (len <= ICMP_ONE_FRAME) {
        /* One pass: copy into txbuf while summing the request for verification. */
        out = mt_ip_payload(s);
        t = mt_cksum_copy(0, out, pl, len);
        if (mt_cksum_fold(t) != 0) { MT_DROP_IP(s); return; }
    } else if (pl == s->reasm->buf) {
        /* Larger than one frame: only reassembled datagrams get here, and the reassembly
         * slot is free again (inactive) while we run, so the reply is rewritten in place. */
        out = s->reasm->buf;
        if (mt_cksum_fold(mt_cksum_add(0, pl, len)) != 0) { MT_DROP_IP(s); return; }
    } else {
        return;
    }
    s->stats.icmp_in++;
    out[0] = ICMP_ECHO_REPLY;
    t = (uint32_t)(uint16_t)~mt_rd16(out + 2) + 0xF7FFu;
    t = (t & 0xFFFFu) + (t >> 16);
    mt_wr16(out + 2, (uint16_t)~t);
    err0 = tx_fails(s);
    if (out == mt_ip_payload(s) ? mt_ip_output(s, src, 1, len, now) == MT_OK
                                : mt_ip_output_buf(s, src, 1, out, len, now) == MT_OK)
        icmp_counted(s, err0);
}

/* Error message about a datagram we sent: body = embedded original IP header + >= 8 bytes. */
static void icmp_error_in(mt_stack *s, uint8_t type, uint8_t code, const uint8_t *q,
                          uint16_t ql) {
    uint16_t ihl;
    mt_ip4 odst;
    uint16_t lport, rport;
    uint8_t proto;
    if (ql < MT_IP_HDR || (q[0] >> 4) != 4) return;
    ihl = (uint16_t)((q[0] & 15u) << 2);
    if (ihl < MT_IP_HDR || ql < (uint16_t)(ihl + 8u)) return;
    if (!mt_ip_is_local(s, mt_rd32(q + 12))) return;      /* not a datagram we sent */
    proto = q[9];
    if (proto != 6 && proto != 17) return;                /* ICMP-about-ICMP and unknown: ignore */
    odst = mt_rd32(q + 16);
    lport = mt_rd16(q + ihl);
    rport = mt_rd16(q + ihl + 2);
    if (proto == 6) mt_tcp_icmp_error(s, odst, lport, rport, type, code);
    else mt_udp_icmp_error(s, odst, lport, rport, type, code);
}

void mt_icmp_input(mt_stack *s, mt_ip4 src, mt_ip4 dst, const uint8_t *pl, uint16_t len,
                   uint32_t now) {
    uint8_t type;
    if (len < ICMP_HDR || !mt_ip_is_local(s, dst) || !unicast_src(s, src)) {
        MT_DROP_IP(s);
        return;
    }
    type = pl[0];
    if (type == ICMP_ECHO) {                              /* verifies the checksum itself */
        icmp_echo_reply(s, src, pl, len, now);
        return;
    }
    if (mt_cksum_fold(mt_cksum_add(0, pl, len)) != 0) { MT_DROP_IP(s); return; }
    s->stats.icmp_in++;
    switch (type) {
    case ICMP_ECHO_REPLY:
        if (s->echo_cb)
            s->echo_cb(s->echo_user, src, mt_rd16(pl + 4), mt_rd16(pl + 6), pl + ICMP_HDR,
                       (uint16_t)(len - ICMP_HDR), now);
        break;
    case ICMP_UNREACH: case ICMP_SQUENCH: case ICMP_TIMEEXC: case ICMP_PARAMPROB:
        icmp_error_in(s, type, pl[1], pl + ICMP_HDR, (uint16_t)(len - ICMP_HDR));
        break;
    default:
        break;
    }
}

int mt_icmp_echo(mt_stack *s, mt_ip4 dst, uint16_t id, uint16_t seq, const uint8_t *data,
                 uint16_t len, uint32_t now) {
    uint8_t *m = mt_ip_payload(s);
    uint32_t t;
    uint32_t err0 = tx_fails(s);
    int r;
    if (len > ICMP_ONE_FRAME - ICMP_HDR || (len && !data)) return MT_ERR_INVAL;
    m[0] = ICMP_ECHO; m[1] = 0; m[2] = 0; m[3] = 0;
    mt_wr16(m + 4, id);
    mt_wr16(m + 6, seq);
    t = mt_cksum_add(0, m, ICMP_HDR);                     /* header is even: chaining is safe */
    t = mt_cksum_copy(t, m + ICMP_HDR, data, len);
    mt_wr16(m + 2, mt_cksum_fold(t));
    r = mt_ip_output(s, dst, 1, (uint16_t)(ICMP_HDR + len), now);
    if (r == MT_OK) icmp_counted(s, err0);
    return r;
}

void mt_icmp_send_unreach(mt_stack *s, mt_ip4 src, mt_ip4 dst, uint8_t proto, const uint8_t *pl,
                          uint16_t len, uint8_t code, uint32_t now) {
    uint8_t *m = mt_ip_payload(s);
    uint8_t *h = m + ICMP_HDR;
    uint16_t q = len < 8 ? len : 8;
    uint32_t err0;
    /* Never toward one of our own addresses: a loopback datagram to a closed port gets no error
     * (no self-made feedback path, and the limiter below stays the wire's). */
    if (!mt_ip_is_local(s, dst) || !unicast_src(s, src) || mt_ip_is_local(s, src) || proto == 1)
        return;
    /* Error messages we originate: at most one per ICMP_ERR_GAP_MS per stack (RFC 1812 4.3.2.8). */
    /* Suppress only while 0 < next - now <= GAP: 0 (none yet) or an ancient `next` never blocks. */
    if (s->icmp_err_next && (uint32_t)(s->icmp_err_next - now - 1u) < ICMP_ERR_GAP_MS) return;
    s->icmp_err_next = now + ICMP_ERR_GAP_MS;
    if (s->icmp_err_next == 0) s->icmp_err_next = 1;                 /* 0 means "none yet" */
    memset(m, 0, ICMP_HDR);
    m[0] = ICMP_UNREACH; m[1] = code;
    /* Synthesised original header (see mt_int.h). */
    h[0] = 0x45; h[1] = 0;
    mt_wr16(h + 2, (uint16_t)(MT_IP_HDR + len));
    mt_wr16(h + 4, 0); mt_wr16(h + 6, 0);
    h[8] = 1; h[9] = proto;
    mt_wr16(h + 10, 0);
    mt_wr32(h + 12, src);
    mt_wr32(h + 16, dst);
    mt_wr16(h + 10, mt_cksum_fold(mt_cksum_add(0, h, MT_IP_HDR)));
    memcpy(h + MT_IP_HDR, pl, q);
    mt_wr16(m + 2, mt_cksum_fold(mt_cksum_add(0, m, ICMP_HDR + MT_IP_HDR + (uint32_t)q)));
    err0 = tx_fails(s);
    if (mt_ip_output(s, src, 1, (uint16_t)(ICMP_HDR + MT_IP_HDR + q), now) == MT_OK)
        icmp_counted(s, err0);
}
