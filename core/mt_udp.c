#include <string.h>
#include "mt_int.h"

/* UDP (RFC 768). Endpoints live in the arena (slot free = port 0); receive buffers belong
 * to the caller. Wire/record fields are accessed bytewise (mt_rd/mt_wr): neither frames
 * nor application buffers have any alignment guarantee. */

#define UDP_HDR 8u
#define UDP_PROTO 17u
#define UDP_MAX_DATA 65507u
#define UDP_MIN_BUF 1024u
#define UDP_EPH_BASE 49152u
#define UDP_ONE_FRAME (MT_TXBUF_SIZE - MT_ETH_HDR - MT_IP_HDR)

static mt_udp *udp_find(const mt_stack *s, uint16_t port) {
    uint16_t i;
    if (port == 0) return NULL;
    for (i = 0; i < s->lim.max_udp; i++)
        if (s->udp[i].port == port) return &s->udp[i];
    return NULL;
}

/* Partial sum of the pseudo-header: addresses, protocol, UDP length. */
static uint32_t pseudo_sum(mt_ip4 src, mt_ip4 dst, uint16_t ulen) {
    return (src >> 16) + (src & 0xFFFFu) + (dst >> 16) + (dst & 0xFFFFu) + UDP_PROTO + ulen;
}

mt_udp *mt_udp_create(mt_stack *s, uint16_t lport, uint8_t *buf, uint32_t buflen, mt_udp_cb cb,
                      void *user) {
    mt_udp *u = NULL;
    uint16_t i;
    if (!buf || buflen < UDP_MIN_BUF) return NULL;
    for (i = 0; i < s->lim.max_udp; i++)
        if (s->udp[i].port == 0) { u = &s->udp[i]; break; }
    if (!u) return NULL;
    if (lport == 0) {
        uint32_t tries;
        uint16_t p = s->udp_eph;
        for (tries = 0; tries < 65536u - UDP_EPH_BASE; tries++) {
            if (p < UDP_EPH_BASE) p = UDP_EPH_BASE;
            if (!udp_find(s, p)) break;
            p = (uint16_t)(p == 65535u ? UDP_EPH_BASE : p + 1u);
        }
        if (tries == 65536u - UDP_EPH_BASE) return NULL;
        lport = p;
        s->udp_eph = (uint16_t)(p == 65535u ? UDP_EPH_BASE : p + 1u);
    } else if (udp_find(s, lport)) {
        return NULL;
    }
    memset(u, 0, sizeof *u);
    u->st = s;
    u->buf = buf;
    u->buflen = buflen;
    u->cb = cb;
    u->user = user;
    u->port = lport;
    return u;
}

uint16_t mt_udp_port(const mt_udp *u) { return u->port; }

void mt_udp_release(mt_udp *u) { memset(u, 0, sizeof *u); }

uint32_t mt_udp_queued(const mt_udp *u) { return u->n; }

int mt_udp_peek(mt_udp *u, mt_udp_dgram *out) {
    const uint8_t *r;
    if (u->n == 0) return 0;
    r = u->buf + u->head;
    out->src = mt_rd32(r);
    out->sport = mt_rd16(r + 4);
    out->len = mt_rd16(r + 6);
    out->data = r + UDP_HDR;
    return 1;
}

void mt_udp_done(mt_udp *u) {
    if (u->n == 0) return;
    u->head += (UDP_HDR + (uint32_t)mt_rd16(u->buf + u->head + 6) + 1u) & ~1u;
    if (--u->n == 0) {
        u->head = u->tail = u->wrap = 0;
    } else if (u->wrap && u->head >= u->wrap) {
        u->head = 0;
        u->wrap = 0;
    }
}

/* Offset where a record of r bytes can go, or UINT32_MAX if it does not fit. Wraps the
 * tail to 0 (recording the wrap point) when the run up to the end is too short. */
static uint32_t slot_for(mt_udp *u, uint32_t r) {
    if (u->n == 0) {
        u->head = u->tail = u->wrap = 0;
        return r <= u->buflen ? 0 : 0xFFFFFFFFu;
    }
    if (u->wrap == 0) {                      /* data runs head..tail */
        if (u->tail + r <= u->buflen) return u->tail;
        if (r <= u->head) { u->wrap = u->tail; u->tail = 0; return 0; }
        return 0xFFFFFFFFu;
    }
    return u->tail + r <= u->head ? u->tail : 0xFFFFFFFFu;   /* data runs 0..tail, head..wrap */
}

void mt_udp_input(mt_stack *s, mt_ip4 src, mt_ip4 dst, const uint8_t *pl, uint16_t len,
                  uint32_t now) {
    mt_udp *u;
    uint16_t ulen, dlen, why;
    uint32_t r, off, sum = 0;
    uint8_t *rec;
    int checked;

    if (len < UDP_HDR) MT_DROP_GOTO(why);
    ulen = mt_rd16(pl + 4);
    if (ulen < UDP_HDR || ulen > len) MT_DROP_GOTO(why);
    dlen = (uint16_t)(ulen - UDP_HDR);
    checked = mt_rd16(pl + 6) != 0;
    u = udp_find(s, mt_rd16(pl + 2));

    if (!u) {
        if (checked &&
            mt_cksum_fold(mt_cksum_add(pseudo_sum(src, dst, ulen), pl, ulen)) != 0) MT_DROP_GOTO(why);
        s->stats.udp_noport++;
        if (dst == s->cfg.addr) mt_icmp_send_unreach(s, src, dst, UDP_PROTO, pl, ulen, 3, now);
        return;
    }

    r = (UDP_HDR + (uint32_t)dlen + 1u) & ~1u;
    off = slot_for(u, r);
    if (off == 0xFFFFFFFFu) MT_DROP_GOTO(why);
    rec = u->buf + off;

    /* One pass: copy the payload into the record while summing it. The record is only
     * committed (tail/n) once the checksum is known good. */
    if (checked) {
        sum = mt_cksum_add(pseudo_sum(src, dst, ulen), pl, UDP_HDR);
        sum = mt_cksum_copy(sum, rec + UDP_HDR, pl + UDP_HDR, dlen);
        if (mt_cksum_fold(sum) != 0) MT_DROP_GOTO(why);
    } else if (dlen) {
        memcpy(rec + UDP_HDR, pl + UDP_HDR, dlen);
    }
    mt_wr32(rec, src);
    mt_wr16(rec + 4, mt_rd16(pl));
    mt_wr16(rec + 6, dlen);
    u->tail = off + r;
    u->n++;
    s->stats.udp_in++;
    if (u->cb) u->cb(u, MT_UDP_EV_DATA, 0, u->user);
    return;
drop:
    s->stats.udp_drops++;
    MT_LOG_DROPPED(s, MT_LOGL_UDP, why);
}

void mt_udp_icmp_error(mt_stack *s, mt_ip4 dst, uint16_t lport, uint16_t rport, uint8_t type,
                       uint8_t code) {
    mt_udp *u = udp_find(s, lport);
#ifdef MT_TEST_HOOKS
    if (s->icmp_err_spy) s->icmp_err_spy(s->icmp_err_ctx, 17, dst, lport, rport, type, code);
#endif
    (void)dst; (void)rport;
    if (u && u->cb) u->cb(u, MT_UDP_EV_ICMP, ((uint32_t)type << 8) | code, u->user);
}

int mt_udp_send(mt_udp *u, mt_ip4 dst, uint16_t dport, const uint8_t *p, uint16_t len,
                int checksum, uint32_t now) {
    mt_stack *s = u->st;
    uint8_t hb[UDP_HDR];
    uint8_t *h;
    uint32_t sum = 0;
    uint16_t ulen = (uint16_t)(UDP_HDR + len), c = 0;
    int big = (uint32_t)UDP_HDR + len > UDP_ONE_FRAME;
    int r;

    if (len > UDP_MAX_DATA || (len && !p)) return MT_ERR_INVAL;
    if (s->cfg.addr == 0) return MT_ERR_UNCONFIGURED;
    /* Small datagrams are built in place in txbuf; big ones keep the header in hb and are
     * gathered into fragments by the IP layer (no contiguous copy exists). */
    h = big ? hb : mt_ip_payload(s);
    mt_wr16(h, u->port);
    mt_wr16(h + 2, dport);
    mt_wr16(h + 4, ulen);
    mt_wr16(h + 6, 0);
    if (checksum) {
        sum = mt_cksum_add(pseudo_sum(s->cfg.addr, dst, ulen), h, UDP_HDR);
        sum = big ? mt_cksum_add(sum, p, len) : mt_cksum_copy(sum, h + UDP_HDR, p, len);
        c = mt_cksum_fold(sum);
        if (c == 0) c = 0xFFFFu;
        mt_wr16(h + 6, c);
    } else if (!big && len) {
        memcpy(h + UDP_HDR, p, len);
    }
    r = big ? mt_ip_output_hdr(s, dst, UDP_PROTO, hb, UDP_HDR, p, len, now)
            : mt_ip_output(s, dst, UDP_PROTO, ulen, now);
    if (r == MT_OK) s->stats.udp_out++;
    return r;
}
