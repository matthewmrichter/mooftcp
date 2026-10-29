#include <string.h>
#include "mt_int.h"

/* ---- Input ---- */

static void ip_dispatch(mt_stack *s, uint8_t proto, mt_ip4 src, mt_ip4 dst, const uint8_t *pl,
                        uint16_t len, uint32_t now) {
#ifdef MT_TEST_HOOKS
    if (s->ip_spy) s->ip_spy(s->ip_spy_ctx, proto, src, dst, pl, len);
#endif
    switch (proto) {
    case 6:  mt_tcp_input(s, src, dst, pl, len, now); break;
    case 17: mt_udp_input(s, src, dst, pl, len, now); break;
    case 1:  mt_icmp_input(s, src, dst, pl, len, now); break;
    default: break;
    }
}

static void reasm_drop(mt_stack *s) {
    s->reasm->active = 0;
    s->stats.ip_reasm_fail++;
}

/* Slow path: fold one fragment into the single reassembly slot. `off` and `dlen`
 * are byte values. Every refusal counts the frame in ip_in_drops. */
static void ip_reasm(mt_stack *s, mt_ip4 src, mt_ip4 dst, uint8_t proto, uint16_t id,
                     uint16_t off, int more, const uint8_t *pl, uint16_t dlen, uint32_t now) {
    mt_reasm *r = s->reasm;
    uint32_t end = (uint32_t)off + dlen;
    uint32_t u, u0, u1, set = 0;

    if (dlen == 0 || (more && (dlen & 7u))) { MT_DROP_IP(s); return; }
    mt_ip_timers(s, now);                      /* lazily expire a stale slot */
    if (end > MT_REASM_MAX) {                  /* never fits: refuse (and kill a matching slot) */
        MT_DROP_IP(s);
        if (r->active && r->src == src && r->id == id && r->proto == proto) reasm_drop(s);
        else s->stats.ip_reasm_fail++;
        return;
    }
    if (!r->active) {
        memset(r->bitmap, 0, sizeof r->bitmap);
        r->t_start = now; r->src = src; r->id = id; r->proto = proto;
        r->got = 0; r->total = 0; r->maxend = 0; r->active = 1;
    } else if (r->src != src || r->id != id || r->proto != proto) {
        MT_DROP_IP(s);                /* slot busy with another datagram */
        s->stats.ip_reasm_fail++;
        return;
    }
    u0 = off >> 3;
    u1 = (end + 7u) >> 3;
    for (u = u0; u < u1; u++)
        if (r->bitmap[u >> 5] & ((uint32_t)1 << (u & 31u))) set++;
    if (set == u1 - u0 && (more || r->total == end)) {   /* exact duplicate: ignore this frame */
        MT_DROP_IP(s);
        return;
    }
    if (set) goto bad;                                   /* partial overlap */
    if (!more) {
        if (r->total || r->maxend > end) goto bad;                 /* second tail / data past tail */
        r->total = (uint16_t)end;
    } else if (r->total && end > r->total) {
        goto bad;
    }
    for (u = u0; u < u1; u++) r->bitmap[u >> 5] |= (uint32_t)1 << (u & 31u);
    r->got = (uint16_t)(r->got + (u1 - u0));
    if (end > r->maxend) r->maxend = (uint16_t)end;
    memcpy(r->buf + off, pl, dlen);
    if (r->total && r->got == (uint16_t)((r->total + 7u) >> 3)) {
        r->active = 0;
        s->stats.ip_reasm_ok++;
        ip_dispatch(s, proto, src, dst, r->buf, r->total, now);
    }
    return;
bad:
    MT_DROP_IP(s);
    reasm_drop(s);
}

void mt_ip_input(mt_stack *s, const uint8_t *ip, uint16_t len, uint32_t now) {
    uint16_t ihl, tot, ff, why;
    mt_ip4 dst;
    if (len < MT_IP_HDR || (ip[0] >> 4) != 4) MT_DROP_GOTO(why);
    ihl = (uint16_t)((ip[0] & 15u) << 2);
    if (ihl < MT_IP_HDR || ihl > len) MT_DROP_GOTO(why);
    tot = mt_rd16(ip + 2);
    if (tot < ihl || tot > len) MT_DROP_GOTO(why);
    if (mt_cksum_fold(mt_cksum_add(0, ip, ihl)) != 0) MT_DROP_GOTO(why);
    dst = mt_rd32(ip + 16);
    if (s->cfg.addr == 0 ||
        (dst != s->cfg.addr && dst != MT_ETH_BCAST_IP && dst != (s->cfg.addr | ~s->cfg.mask)))
        MT_DROP_GOTO(why);
    s->stats.ip_in++;
    ff = mt_rd16(ip + 6);
    if ((ff & 0x3FFFu) == 0) {                                      /* fast path: whole datagram */
        ip_dispatch(s, ip[9], mt_rd32(ip + 12), dst, ip + ihl, (uint16_t)(tot - ihl), now);
    } else {
        ip_reasm(s, mt_rd32(ip + 12), dst, ip[9], mt_rd16(ip + 4),
                 (uint16_t)((ff & 0x1FFFu) << 3), (ff & 0x2000u) != 0, ip + ihl,
                 (uint16_t)(tot - ihl), now);
    }
    return;
drop:
    s->stats.ip_in_drops++;
    MT_LOG_DROPPED(s, MT_LOGL_IP, why);
}

/* ---- Reassembly timer ---- */

void mt_ip_timers(mt_stack *s, uint32_t now) {
    mt_reasm *r = s->reasm;
    if (r->active && !mt_time_before(now, r->t_start + MT_REASM_TIMEOUT_MS)) reasm_drop(s);
}

uint32_t mt_ip_deadline(const mt_stack *s, uint32_t now) {
    const mt_reasm *r = s->reasm;
    uint32_t due;
    if (!r->active) return MT_NO_DEADLINE;
    due = r->t_start + MT_REASM_TIMEOUT_MS;
    return mt_time_before(now, due) ? due - now : 0;
}

/* ---- Output ---- */

static void ip_hdr(mt_stack *s, mt_ip4 dst, uint8_t proto, uint16_t id, uint16_t ff,
                   uint16_t seglen) {
    uint8_t *h = s->txbuf + MT_ETH_HDR;
    h[0] = 0x45;
    h[1] = 0;
    mt_wr16(h + 2, (uint16_t)(MT_IP_HDR + seglen));
    mt_wr16(h + 4, id);
    mt_wr16(h + 6, ff);
    h[8] = 64;
    h[9] = proto;
    h[10] = 0; h[11] = 0;
    mt_wr32(h + 12, s->cfg.addr);
    mt_wr32(h + 16, dst);
    mt_wr16(h + 10, mt_cksum_fold(mt_cksum_add(0, h, MT_IP_HDR)));
}

/* Copy n bytes of the virtual payload hdr[0..hlen) || pl, starting at offset vo, to body. */
static void ip_gather(uint8_t *body, const uint8_t *hdr, uint32_t hlen, const uint8_t *pl,
                      uint32_t vo, uint16_t n) {
    if (vo < hlen) {
        uint16_t k = (uint16_t)(hlen - vo);
        if (k > n) k = n;
        memcpy(body, hdr + vo, k);
        body += k;
        n = (uint16_t)(n - k);
    } else {
        pl += vo - hlen;
    }
    if (n && pl != body) memmove(body, pl, n);   /* in-place source: earlier data already sent */
}

int mt_ip_output_hdr(mt_stack *s, mt_ip4 dst, uint8_t proto, const uint8_t *hdr, uint16_t hlen,
                     const uint8_t *pl, uint16_t plen, uint32_t now) {
    mt_ip4 nexthop;
    uint16_t id, mtu, chunk;
    uint32_t off, tot = (uint32_t)hlen + plen;
    uint8_t *body = s->txbuf + MT_ETH_HDR + MT_IP_HDR;

    if (s->cfg.addr == 0) return MT_ERR_UNCONFIGURED;
    if (dst == MT_ETH_BCAST_IP || dst == (s->cfg.addr | ~s->cfg.mask)) nexthop = MT_ETH_BCAST_IP;
    else if (((dst ^ s->cfg.addr) & s->cfg.mask) == 0) nexthop = dst;
    else if (s->cfg.gateway) nexthop = s->cfg.gateway;
    else return MT_ERR_NOROUTE;

    mtu = s->cfg.mtu;
    if (mtu > MT_TXBUF_SIZE - MT_ETH_HDR) mtu = (uint16_t)(MT_TXBUF_SIZE - MT_ETH_HDR);
    if (tot > 65535u - MT_IP_HDR) return MT_ERR_INVAL;
    if (MT_IP_HDR + tot > mtu && mtu < MT_IP_HDR + 8u) return MT_ERR_INVAL;   /* cannot fragment */
    id = s->ip_id++;
    s->stats.ip_out++;

    if (MT_IP_HDR + tot <= mtu) {                                    /* fast path */
        ip_gather(body, hdr, hlen, pl, 0, (uint16_t)tot);
        ip_hdr(s, dst, proto, id, 0, (uint16_t)tot);
        mt_eth_send(s, nexthop, 0x0800, (uint16_t)(MT_IP_HDR + tot), now);
        return MT_OK;
    }

    chunk = (uint16_t)(((mtu - MT_IP_HDR) >> 3) << 3);
    for (off = 0; off < tot; off += chunk) {   /* off is 32-bit: no wrap near 65535 */
        uint16_t n = (uint16_t)(tot - off);
        uint16_t ff = (uint16_t)(off >> 3);
        if (n > chunk) { n = chunk; ff |= 0x2000u; }
        ip_gather(body, hdr, hlen, pl, off, n);
        ip_hdr(s, dst, proto, id, ff, n);
        s->stats.ip_frag_out++;
        /* Fragments are too big to be held for ARP: stop once one was not sent. */
        if (mt_eth_send(s, nexthop, 0x0800, (uint16_t)(MT_IP_HDR + n), now) != 1) break;
    }
    return MT_OK;
}

int mt_ip_output_buf(mt_stack *s, mt_ip4 dst, uint8_t proto, const uint8_t *pl, uint16_t plen,
                     uint32_t now) {
    return mt_ip_output_hdr(s, dst, proto, NULL, 0, pl, plen, now);
}

int mt_ip_output(mt_stack *s, mt_ip4 dst, uint8_t proto, uint16_t plen, uint32_t now) {
    if (plen > MT_TXBUF_SIZE - MT_ETH_HDR - MT_IP_HDR) return MT_ERR_INVAL;
    return mt_ip_output_buf(s, dst, proto, mt_ip_payload(s), plen, now);
}
