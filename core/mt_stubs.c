#include "mt_int.h"

/* Placeholders for functions implemented by later tasks. Each task deletes its
 * stub from this file when it adds the real definition. */

/* Task 7 replaces this. Under MT_TEST_HOOKS they report to icmp_err_spy. */
void mt_tcp_icmp_error(mt_stack *s, mt_ip4 dst, uint16_t lport, uint16_t rport, uint8_t type,
                       uint8_t code) {
#ifdef MT_TEST_HOOKS
    if (s->icmp_err_spy) s->icmp_err_spy(s->icmp_err_ctx, 6, dst, lport, rport, type, code);
#else
    (void)s; (void)dst; (void)lport; (void)rport; (void)type; (void)code;
#endif
}
/* Task 7 (core/mt_tcp.c) replaces these. */
void mt_tcp_input(mt_stack *s, mt_ip4 src, mt_ip4 dst, const uint8_t *pl, uint16_t len, uint32_t now) {
    (void)s; (void)src; (void)dst; (void)pl; (void)len; (void)now;
}
void mt_tcp_arp_resolved(mt_stack *s, mt_ip4 ip, uint32_t now) {
    (void)s; (void)ip; (void)now;
}
