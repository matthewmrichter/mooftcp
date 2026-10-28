#include "mt_int.h"

/* Placeholders for functions implemented by later tasks. Each task deletes its
 * stub from this file when it adds the real definition. */

/* Task 5 (ICMP) replaces this. */
void mt_icmp_input(mt_stack *s, mt_ip4 src, mt_ip4 dst, const uint8_t *pl, uint16_t len, uint32_t now) {
    (void)s; (void)src; (void)dst; (void)pl; (void)len; (void)now;
}

/* Task 6 (UDP) replaces this. */
void mt_udp_input(mt_stack *s, mt_ip4 src, mt_ip4 dst, const uint8_t *pl, uint16_t len, uint32_t now) {
    (void)s; (void)src; (void)dst; (void)pl; (void)len; (void)now;
}

/* Task 7 (core/mt_tcp.c) replaces these. */
void mt_tcp_input(mt_stack *s, mt_ip4 src, mt_ip4 dst, const uint8_t *pl, uint16_t len, uint32_t now) {
    (void)s; (void)src; (void)dst; (void)pl; (void)len; (void)now;
}
void mt_tcp_arp_resolved(mt_stack *s, mt_ip4 ip, uint32_t now) {
    (void)s; (void)ip; (void)now;
}
