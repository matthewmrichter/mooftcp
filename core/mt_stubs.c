#include "mt_int.h"

/* Placeholders for functions implemented by later tasks. Each task deletes its
 * stub from this file when it adds the real definition. */

/* Task 4 (core/mt_ip.c) replaces this. */
void mt_ip_input(mt_stack *s, const uint8_t *ip, uint16_t len, uint32_t now) {
    (void)s; (void)ip; (void)len; (void)now;
}

/* Task 7 (core/mt_tcp.c) replaces this. */
void mt_tcp_arp_resolved(mt_stack *s, mt_ip4 ip, uint32_t now) {
    (void)s; (void)ip; (void)now;
}
