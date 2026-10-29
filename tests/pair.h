#ifndef PAIR_H
#define PAIR_H

#include <stdint.h>
#include "mt.h"

/* Two real stacks, A (192.168.50.200) and B (192.168.50.10), joined by a faulty link.
 * Each has its own arena and tx path; they share one virtual clock. Frames are queued with
 * a delivery time (now + delay_ms + optional extra) and handed to the peer's mt_input by
 * pair_run. Faults are applied per frame at transmit time, from a deterministic xorshift32
 * PRNG seeded with `seed` (0 behaves as 1). Rates are parts per million.
 *   drop    frame vanishes.
 *   dup     frame is delivered twice (the copy gets its own extra delay).
 *   reorder frame gets 1..20 ms of extra delay, so later frames overtake it.
 *   corrupt one random byte is XORed with a random nonzero value. Corruption inside the
 *           IPv4 packet is delivered (IP/TCP checksums must reject it). Corruption of an
 *           ARP frame or of the Ethernet header is modelled as an Ethernet FCS failure and
 *           the frame is dropped (a real NIC never hands such a frame up). */
typedef struct { uint32_t drop_ppm, dup_ppm, reorder_ppm, corrupt_ppm, delay_ms, seed; } pair_faults;
typedef struct pair pair;

#define PAIR_A_IP 0xC0A832C8u   /* 192.168.50.200 */
#define PAIR_B_IP 0xC0A8320Au   /* 192.168.50.10  */

pair    *pair_new(const pair_faults *f, uint32_t start_ms);
void     pair_free(pair *p);
mt_stack *pair_a(pair *p);
mt_stack *pair_b(pair *p);
/* Advance the virtual clock by ms, delivering frames and servicing deadlines (mt_tick is
 * called only when mt_next_deadline reports 0). Everything due at or before the new time
 * has been handled when it returns. */
void     pair_run(pair *p, uint32_t ms);
uint32_t pair_now(const pair *p);
/* Directed-fault hook, consulted for every transmitted frame before the random faults:
 * returning nonzero drops the frame (from: 0 = sent by A, 1 = sent by B). NULL clears it. */
typedef int (*pair_filter_fn)(void *user, int from, const uint8_t *frame, uint16_t len);
void     pair_set_filter(pair *p, pair_filter_fn fn, void *user);
/* Number of times mt_tick left a stack with an already-expired deadline (contract
 * violation; expected 0). */
uint32_t pair_stuck(const pair *p);
/* Frames put on the wire / dropped by the link model / delivered corrupted. */
uint32_t pair_sent(const pair *p);
uint32_t pair_dropped(const pair *p);
uint32_t pair_corrupted(const pair *p);

#endif
