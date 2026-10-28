#ifndef NET_H
#define NET_H

#include <stdint.h>
#include "mt.h"

/* Fake wire: tx() copies every transmitted frame into f[]/len[] (drops, and
 * returns nonzero, once 64 are queued). */
typedef struct { uint8_t f[64][1514]; uint16_t len[64]; int n; } fake_link;
void fake_init(fake_link *fl, mt_link *link);
void fake_clear(fake_link *fl);

#define T_ME   0xC0A832C8u   /* 192.168.50.200 */
#define T_GW   0xC0A83201u
#define T_PEER 0xC0A8320Au
#define T_FAR  0x08080808u
extern const uint8_t T_ME_MAC[6], T_PEER_MAC[6], T_GW_MAC[6];

/* Stack on a static arena (pool of 4, reused round-robin): /24, gateway .1,
 * MAC 02:00:00:00:00:01, mtu 1500, seed 1. addr==0 gives an unconfigured stack. */
mt_stack *tstack_new(fake_link *fl, mt_ip4 addr);

/* Frame builders; each returns the total frame length. For ARP, eth dst is
 * broadcast when op==1 and tmac otherwise; eth src is smac. */
uint16_t mk_arp(uint8_t *f, uint16_t op, const uint8_t smac[6], mt_ip4 sip,
                const uint8_t tmac[6], mt_ip4 tip);
/* Eth+IPv4 (IHL 5, TTL 64, valid checksum). eth dst = our MAC; eth src =
 * T_GW_MAC if src is T_GW or off-subnet, else T_PEER_MAC. frag_off_flags is
 * the raw 16-bit flags/offset field. */
uint16_t mk_ip(uint8_t *f, mt_ip4 src, mt_ip4 dst, uint8_t proto, uint16_t id,
               uint16_t frag_off_flags, const uint8_t *pl, uint16_t plen);

typedef struct {
    uint16_t op; uint8_t smac[6]; mt_ip4 sip; uint8_t tmac[6]; mt_ip4 tip;
    uint8_t eth_dst[6], eth_src[6];
} arpv;
typedef struct {
    mt_ip4 src, dst; uint8_t proto; uint16_t id;
    uint16_t flags;   /* top 3 bits of the frag field, unshifted (0x4000 = DF, 0x2000 = MF) */
    uint16_t frag;    /* fragment offset in 8-byte units */
    const uint8_t *payload; uint16_t len;
    uint8_t eth_dst[6], eth_src[6];
} ipv;
/* Return 1 if f is a well-formed Ethernet+ARP/IPv4 frame, filling *out. */
int view_arp(const uint8_t *f, uint16_t len, arpv *out);
int view_ip(const uint8_t *f, uint16_t len, ipv *out);

#endif
