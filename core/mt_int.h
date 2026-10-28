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
 *     = running total, then total += MT_ALIGN4(bytes)) and its pointer
 *     assignment to mt_stack_init(). mt_stack_size() and mt_stack_init() both
 *     use mt_layout_calc(), so the size accounting stays authoritative.
 *  2. Timers: implement `uint32_t mt_<mod>_deadline(const mt_stack*, uint32_t now)`
 *     returning ms from now (0 = overdue) or MT_NO_DEADLINE, and
 *     `void mt_<mod>_timers(mt_stack*, uint32_t now)`. Then add ONE line each to
 *     mt_next_deadline() (`d = mt_dl_min(d, mt_<mod>_deadline(s, now));`) and
 *     mt_tick() in mt_stack.c. Plain calls, no registration table.
 *  3. Delete the matching stub from mt_stubs.c.
 */

#define MT_ALIGN4(x) (((x) + 3u) & ~3u)
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

struct mt_stack {
    mt_ifconfig cfg;
    mt_link link;
    mt_limits lim;
    mt_stats stats;
    uint32_t seed;
    /* arena sections */
    mt_arp_entry *arp;       /* lim.arp_entries entries */
    mt_pending *pend;
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

/* ---- Stubs (mt_stubs.c): delete each when its owning task implements it ---- */
void mt_ip_input(mt_stack *s, const uint8_t *ip, uint16_t len, uint32_t now);   /* Task 4 */
void mt_tcp_arp_resolved(mt_stack *s, mt_ip4 ip, uint32_t now);                 /* Task 7 */

#endif
