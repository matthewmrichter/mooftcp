#ifndef MT_H
#define MT_H

#include <stdint.h>

/* MoofTCP portable core: public API. All state lives in one caller-supplied
 * arena; the core never allocates and has no mutable globals. */

typedef uint32_t mt_ip4;                         /* host byte order */
#define MT_NO_DEADLINE 0xFFFFFFFFu

typedef struct mt_stack mt_stack;

/* tx must finish with `frame` before returning; nonzero = dropped */
typedef struct { void *ctx; int (*tx)(void *ctx, const uint8_t *frame, uint16_t len); } mt_link;

typedef struct {
    mt_ip4 addr, mask, gateway, dns;
    char domain[64];
    uint8_t mac[6];
    uint16_t mtu;
} mt_ifconfig;

typedef struct { uint8_t max_tcp, max_udp, arp_entries, dns_cache; } mt_limits;

/* Counters. Task 12 appends the remaining fields (ip_*, icmp_*, udp_*, tcp_*)
 * after these; field names follow the plan. */
typedef struct {
    uint32_t eth_in;      /* frames >= 14 bytes addressed to us or broadcast */
    uint32_t eth_out;     /* frames handed to link.tx successfully */
    uint32_t eth_tx_err;  /* link.tx returned nonzero (or no tx function) */
    uint32_t arp_in;      /* ARP frames received (accepted by the Ethernet filter) */
    uint32_t arp_out;     /* ARP requests/replies transmitted */
    uint32_t arp_drops;   /* unresolved next hops given up on + held frames discarded/replaced */
} mt_stats;

void      mt_limits_default(mt_limits *l);      /* 16, 8, 8, 8 */
uint32_t  mt_stack_size(const mt_limits *l);    /* arena bytes needed */
/* NULL if len < mt_stack_size(l) or mem is not 4-byte aligned. */
mt_stack *mt_stack_init(void *mem, uint32_t len, const mt_limits *l, const mt_link *link,
                        uint32_t seed);
/* A stack with addr==0 sends nothing and answers nothing. */
void      mt_set_config(mt_stack *s, const mt_ifconfig *c);
void      mt_get_config(const mt_stack *s, mt_ifconfig *c);
const mt_stats *mt_get_stats(const mt_stack *s);

void      mt_input(mt_stack *s, const uint8_t *frame, uint16_t len, uint32_t now);
void      mt_tick(mt_stack *s, uint32_t now);
uint32_t  mt_next_deadline(const mt_stack *s, uint32_t now);   /* ms from now, or MT_NO_DEADLINE */

#endif
