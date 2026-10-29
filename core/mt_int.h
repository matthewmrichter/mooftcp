#ifndef MT_INT_H
#define MT_INT_H

#include <stddef.h>
#include "mt.h"
#include "mt_util.h"

/*
 * Internal definitions shared by the core C files.
 *
 * ADDING A MODULE (TCP, UDP, DNS, reassembly, log, ...):
 *  1. Arena: add its size to mt_layout_calc() in mt_stack.c (one line: offset
 *     = running total, then total = mt_arena_up(total + bytes), so the NEXT section
 *     starts MT_ARENA_ALIGN-aligned) and its pointer
 *     assignment to mt_stack_init(). mt_stack_size() and mt_stack_init() both
 *     use mt_layout_calc(), so the size accounting stays authoritative.
 *  2. Timers: implement `uint32_t mt_<mod>_deadline(const mt_stack*, uint32_t now)`
 *     returning ms from now (0 = overdue) or MT_NO_DEADLINE, and
 *     `void mt_<mod>_timers(mt_stack*, uint32_t now)`. Then add ONE line each to
 *     mt_next_deadline() (`d = mt_dl_min(d, mt_<mod>_deadline(s, now));`) and
 *     mt_tick() in mt_stack.c. Plain calls, no registration table.
 *  3. If the module started life as a stub in core/mt_stubs.c, delete the stub.
 */

#define MT_ALIGN4(x) (((x) + 3u) & ~3u)
/* Arena base and every section start are aligned to this: 8 where pointers are 64-bit (host
 * tests), 4 otherwise (68k). */
#define MT_ARENA_ALIGN ((uint32_t)(sizeof(void *) > 4 ? 8u : 4u))
#define MT_ETH_HDR 14u
#define MT_TXBUF_SIZE 1514u
#define MT_ETH_BCAST_IP 0xFFFFFFFFu

/* Smaller of two relative deadlines (MT_NO_DEADLINE is the maximum value). */
static inline uint32_t mt_dl_min(uint32_t a, uint32_t b) { return a < b ? a : b; }

/* ---- ARP ---- */
enum { MT_ARP_FREE = 0, MT_ARP_PENDING = 1, MT_ARP_RESOLVED = 2 };
/* 20 bytes. t_stamp = time of first request (pending) or of resolution. */
typedef struct {
    mt_ip4 ip;
    uint32_t t_used;
    uint32_t t_stamp;
    uint8_t mac[6];
    uint8_t state;
    uint8_t tries;       /* requests sent while pending */
} mt_arp_entry;

/* Single frame held while its next hop resolves (len == 0: empty). Only small
 * frames wait on ARP (SYN, DNS query, ICMP echo, first UDP datagram); larger
 * ones are dropped and recovered by upper-layer retransmission. */
#define MT_PEND_MAX 256u   /* frame bytes including the 14-byte Ethernet header */
typedef struct {
    mt_ip4 nexthop;
    uint16_t len;
    uint8_t frame[MT_PEND_MAX];
} mt_pending;

/* ---- IPv4 reassembly: one slot, payload buffer in the arena ---- */
#define MT_REASM_MAX 4096u
#define MT_REASM_UNITS (MT_REASM_MAX / 8u)       /* 512 bits of 8-byte units */
#define MT_REASM_TIMEOUT_MS 15000u
typedef struct {
    uint32_t bitmap[MT_REASM_UNITS / 32u];
    uint32_t t_start;
    mt_ip4 src;
    uint16_t id;
    uint16_t got;        /* units received */
    uint16_t total;      /* payload length once the last fragment is seen, else 0 */
    uint16_t maxend;     /* highest byte offset covered so far */
    uint8_t proto;
    uint8_t active;
    uint8_t buf[MT_REASM_MAX];
} mt_reasm;

#ifdef MT_TEST_HOOKS
/* Test-only spy: mt_ip_input calls it for every datagram it is about to hand to
 * a protocol handler (including reassembled ones and unknown protocols), then
 * dispatches normally. Never compiled into production builds. */
typedef void (*mt_ip_spy_fn)(void *ctx, uint8_t proto, mt_ip4 src, mt_ip4 dst,
                             const uint8_t *pl, uint16_t len);
#endif

/* ---- UDP endpoint (arena array of lim.max_udp; port 0 = free slot) ----
 * Receive area: records [src(4) sport(2) len(2) data] padded to even, placed contiguously
 * in the caller's buf. head/tail are byte offsets; when a record does not fit before the
 * end, `wrap` remembers where the tail run stopped and the next record starts at 0. */
struct mt_udp {
    mt_stack *st;
    uint8_t *buf;
    uint32_t buflen, head, tail, wrap;
    mt_udp_cb cb;
    void *user;
    uint16_t port;
    uint16_t n;          /* queued datagrams */
};

/* ---- TCP (mt_tcp.c lifecycle + input, mt_tcp_out.c segment output) ----
 * TCBs live in an arena array (lim.max_tcp; used == 0 is a free slot). Sequence variables follow
 * RFC 793. The send ring holds bytes from snd_una on (unacked first, then unsent); the FIN, once
 * sent, occupies the last sequence number but no ring byte. Fields are ordered hot first. */
enum { MT_TT_RTO, MT_TT_DELACK, MT_TT_PERSIST, MT_TT_TW, MT_TT_N };   /* tmr[] slots / tmask bits */
#define MT_TF_ACKNOW   0x01u   /* an ACK is owed: mt_tcp_output sends one if nothing carries it */
#define MT_TF_FINQ     0x02u   /* app closed: send FIN after the queued data */
#define MT_TF_FINSENT  0x04u   /* FIN transmitted (occupies seq snd_nxt-1) */
#define MT_TF_PASSIVE  0x08u   /* SYN_RCVD reached from LISTEN: RST/timeout re-listens */
#define MT_TF_EST      0x10u   /* counted in stats.tcp_conns_open */
#define MT_TF_ULPABORT 0x20u   /* ULP timeout aborts (else only fires the event) */
/* flags2 */
#define MT_T2_ACKHALF  0x01u   /* one full-size segment received and not yet ACKed (ack every 2nd) */
#define MT_T2_PUSHQ    0x02u   /* app pushed: queued data goes out regardless of Nagle */
#define MT_T2_SNDWANT  0x04u   /* a send was short: fire SEND_SPACE when ACKs free ring space */
#define MT_T2_SNDURG   0x08u   /* snd_up is live: segments before it carry URG */
#define MT_T2_RCVURG   0x10u   /* rcv_up is live: unread urgent data may remain */
#define MT_TH_FIN 0x01u
#define MT_TH_SYN 0x02u
#define MT_TH_RST 0x04u
#define MT_TH_PSH 0x08u
#define MT_TH_ACK 0x10u
#define MT_TH_URG 0x20u
#define MT_TCP_DELACK_MS 100u
#define MT_TCP_RTO_INIT 1000u
#define MT_TCP_RTO_MAX  60000u
struct mt_tcp {
    mt_stack *st;
    uint32_t snd_una, snd_nxt, snd_wnd, rcv_nxt;
    mt_ring sndq, rcvq;
    uint32_t snd_wl1, snd_wl2, iss, irs;
    uint32_t rcv_adv;         /* right edge of the window last advertised (receiver SWS) */
    uint32_t rcv_up, snd_up;  /* urgent marks: sequence number just past the urgent data */
    uint32_t tmr[MT_TT_N];    /* absolute ms deadlines, valid where the tmask bit is set */
    uint32_t rto, srtt;       /* ms; srtt 0 until measured (Task 9) */
    uint32_t ulp_start;       /* when the oldest unacked data last made progress */
    uint32_t tnow;            /* last clock value seen (for APIs without a `now` argument) */
    mt_ip4 rip, frip;         /* peer; listen filter */
    mt_tcp_cb cb;
    void *user;
    uint16_t lport, rport, flport, frport;   /* connection; listen filters (0 = any) */
    uint16_t mss, ulp_s;
    uint8_t state, flags, flags2, tmask, used;
    uint8_t gen;              /* bumped on create/release/terminate/abort: stale deferred events are dropped */
};


struct mt_stack {
    mt_ifconfig cfg;
    mt_link link;
    mt_limits lim;
    mt_stats stats;
    uint32_t seed;
    /* arena sections */
    mt_arp_entry *arp;       /* lim.arp_entries entries */
    mt_pending *pend;
    mt_reasm *reasm;
    mt_udp *udp;             /* lim.max_udp endpoints */
    mt_tcp *tcp;             /* lim.max_tcp TCBs */
    uint16_t tcp_eph;        /* next ephemeral TCP port candidate */
    /* The TCP segment (if any) that mt_arp_insert just released from the held slot; consulted
     * and cleared by mt_tcp_arp_resolved so it does not retransmit that same segment. */
    struct { mt_ip4 dst; uint32_t seq; uint16_t sport, dport; uint8_t live; } arp_rel;
    uint16_t udp_eph;        /* next ephemeral port candidate */
    uint16_t ip_id;          /* next IPv4 identification */
    mt_echo_cb echo_cb;
    void *echo_user;
#ifdef MT_TEST_HOOKS
    mt_ip_spy_fn ip_spy;
    void *ip_spy_ctx;
    void (*icmp_err_spy)(void *ctx, uint8_t proto, mt_ip4 dst, uint16_t lport, uint16_t rport,
                         uint8_t type, uint8_t code);
    void *icmp_err_ctx;
#endif
    /* scratch */
    uint8_t txbuf[MT_TXBUF_SIZE];   /* upper layers build at txbuf + 14 */
    uint8_t arpbuf[60];             /* ARP requests/replies, kept apart from txbuf */
};

/* Hand a finished frame to the link; counts eth_out / eth_tx_err. 0 = sent. */
int mt_eth_tx(mt_stack *s, const uint8_t *frame, uint16_t len);

/* ARP (mt_arp.c). `arp` points at the ARP payload (after the Ethernet header). */
void mt_arp_input(mt_stack *s, const uint8_t *arp, uint16_t len, uint32_t now);
/* 1 = resolved (mac_out filled), 0 = pending (request sent, at most 1/s per entry). */
int  mt_arp_resolve(mt_stack *s, mt_ip4 nexthop, uint8_t mac_out[6], uint32_t now);
void mt_arp_insert(mt_stack *s, mt_ip4 ip, const uint8_t mac[6], uint32_t now);
void mt_arp_tick(mt_stack *s, uint32_t now);
uint32_t mt_arp_deadline(const mt_stack *s, uint32_t now);

/* Send the frame whose payload is already at txbuf + 14. Returns 1 = sent,
 * 0 = held in the pending slot awaiting ARP (replacing any older held frame);
 * a frame over MT_PEND_MAX is not held (arp_drops++, ARP request still sent) and also
 * returns 0 so the caller's retransmission logic recovers it,
 * -1 = dropped (unconfigured, oversize, or link error). nexthop
 * 255.255.255.255 goes to the Ethernet broadcast address. */
int mt_eth_send(mt_stack *s, mt_ip4 nexthop, uint16_t ethertype, uint16_t payload_len,
                uint32_t now);

/* ---- IPv4 (mt_ip.c) ---- */
#define MT_IP_HDR 20u
/* `ip` points at the IPv4 header (after the Ethernet header). Validates and
 * dispatches whole datagrams (reassembling fragments). Bad frames: ip_in_drops++. */
void mt_ip_input(mt_stack *s, const uint8_t *ip, uint16_t len, uint32_t now);
/* Upper layers build their payload here (txbuf + 14 + 20, at most 1480 bytes). */
static inline uint8_t *mt_ip_payload(mt_stack *s) { return s->txbuf + MT_ETH_HDR + MT_IP_HDR; }
/* Send plen bytes already placed at mt_ip_payload(). Header: TTL 64, DF clear,
 * next ID. Routes (on-link / broadcast / gateway), fragments when 20+plen > mtu.
 * MT_OK (sent, held for ARP or dropped by the link), MT_ERR_NOROUTE,
 * MT_ERR_UNCONFIGURED, MT_ERR_INVAL. */
int mt_ip_output(mt_stack *s, mt_ip4 dst, uint8_t proto, uint16_t plen, uint32_t now);
/* Same, with the payload in caller memory `pl` (any size up to 65515); the only way
 * to send datagrams larger than txbuf. pl may equal mt_ip_payload(). */
int mt_ip_output_buf(mt_stack *s, mt_ip4 dst, uint8_t proto, const uint8_t *pl, uint16_t plen,
                     uint32_t now);
/* Same, with the payload being hdr[0..hlen) followed by pl[0..plen) (hlen + plen <= 65515);
 * lets UDP send big datagrams without a contiguous copy. */
int mt_ip_output_hdr(mt_stack *s, mt_ip4 dst, uint8_t proto, const uint8_t *hdr, uint16_t hlen,
                     const uint8_t *pl, uint16_t plen, uint32_t now);
static inline mt_ip4 mt_ip_src_for(const mt_stack *s) { return s->cfg.addr; }
void mt_ip_timers(mt_stack *s, uint32_t now);
uint32_t mt_ip_deadline(const mt_stack *s, uint32_t now);

/* Protocol handlers. */
void mt_icmp_input(mt_stack *s, mt_ip4 src, mt_ip4 dst, const uint8_t *pl, uint16_t len, uint32_t now);
void mt_udp_input(mt_stack *s, mt_ip4 src, mt_ip4 dst, const uint8_t *pl, uint16_t len, uint32_t now);
void mt_tcp_input(mt_stack *s, mt_ip4 src, mt_ip4 dst, const uint8_t *pl, uint16_t len, uint32_t now);

/* ---- ICMP (mt_icmp.c) ---- */
/* Called for ICMP dest-unreachable (3), source-quench (4), time-exceeded (11) and
 * parameter-problem (12) about a datagram WE sent. dst/lport/rport come from the
 * embedded original header (dst = original destination, lport = original source port,
 * rport = original destination port). */
void mt_tcp_icmp_error(mt_stack *s, mt_ip4 dst, uint16_t lport, uint16_t rport, uint8_t type,
                       uint8_t code);
void mt_udp_icmp_error(mt_stack *s, mt_ip4 dst, uint16_t lport, uint16_t rport, uint8_t type,
                       uint8_t code);
/* Send an ICMP error (type 3, `code`) about a received datagram src->dst whose IP payload is
 * pl/len (the IP header is not available at this layer, so a 20-byte header is synthesised:
 * ID 0, TTL 1, checksum valid; peers match on addresses, protocol and the 8 payload bytes).
 * Nothing is sent unless dst is our own unicast address and src is a unicast peer, so
 * broadcast/multicast datagrams never provoke errors. UDP closed port: code 3. */
void mt_icmp_send_unreach(mt_stack *s, mt_ip4 src, mt_ip4 dst, uint8_t proto, const uint8_t *pl,
                          uint16_t len, uint8_t code, uint32_t now);

/* ---- TCP internals ---- */
void mt_tcp_timers(mt_stack *s, uint32_t now);
uint32_t mt_tcp_deadline(const mt_stack *s, uint32_t now);
/* An ARP entry for `ip` just resolved: every connection whose next hop is `ip` and that has
 * unacked data retransmits its first unacked segment, except the one mt_arp_insert released
 * from the held slot a moment ago (recorded in s->arp_rel via mt_tcp_note_released). */
void mt_tcp_arp_resolved(mt_stack *s, mt_ip4 ip, uint32_t now);
void mt_tcp_note_released(mt_stack *s, const uint8_t *iphdr);   /* held frame about to be sent */
/* Transmit whatever the state permits: SYN / SYN-ACK, queued data within the peer window,
 * FIN, or a pending ACK. Single decision point; every sender goes through it. */
void mt_tcp_output(mt_tcp *t, uint32_t now);
/* Window to advertise now: the receive ring's free space, except that growth smaller than
 * min(mss, ring/2) over the last advertised edge is withheld (receiver SWS avoidance). With
 * `commit` the advertised edge is recorded. Never shrinks an advertised window. */
uint32_t mt_tcp_rcv_window(mt_tcp *t, int commit);
/* Build and send one segment of t: seq, flags (ACK is added/ack filled from rcv_nxt when the
 * MT_TH_ACK bit is set), n payload bytes copied from the send ring at offset off. */
void mt_tcp_xmit(mt_tcp *t, uint32_t seq, uint8_t flags, uint32_t off, uint32_t n, uint32_t now);
/* Retransmit the first unacked segment (SYN, SYN-ACK, data from the ring, or FIN). */
void mt_tcp_rexmit(mt_tcp *t, uint32_t now);
/* Segment without a TCB (reset replies) or with explicit fields. */
void mt_tcp_send_raw(mt_stack *s, mt_ip4 rip, uint16_t lport, uint16_t rport, uint32_t seq,
                     uint32_t ack, uint8_t flags, uint32_t now);
uint16_t mt_tcp_local_mss(const mt_stack *s);
static inline uint32_t mt_tcp_pseudo(mt_ip4 src, mt_ip4 dst, uint16_t len) {
    return (src >> 16) + (src & 0xFFFFu) + (dst >> 16) + (dst & 0xFFFFu) + 6u + len;
}

#endif
