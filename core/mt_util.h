#ifndef MT_UTIL_H
#define MT_UTIL_H

#include <stdint.h>

/* Big-endian packet access on byte pointers (no alignment or aliasing assumptions). */
static inline uint16_t mt_rd16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static inline uint32_t mt_rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline void mt_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static inline void mt_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Wrap-safe comparison of ms clock values and TCP sequence numbers. */
static inline int mt_time_before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline int mt_seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline int mt_seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static inline int mt_seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }
static inline int mt_seq_ge(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

/* Internet checksum (RFC 1071). Bytes are summed as big-endian 16-bit words counted from p
 * (not from the address parity); an odd trailing byte is the high byte of a word, so only the
 * last chunk of a chained sum may have odd length. The result is canonical: the 16-bit
 * ones-complement sum of the carried-in sum (any uint32) and the words, in 0..0xFFFF, 0 only if
 * the sum and all bytes are 0, otherwise in 1..0xFFFF. Callers may add results (and chain them
 * as the next sum) freely. Precondition: len <= 65535. The Mac build replaces mt_cksum_add and
 * mt_cksum_copy with 68000 asm (MT_ASM_CKSUM, mac/glue/mt_cksum68k.s) that returns
 * bit-identical results; tests/test_util.c pins the contract. */
uint32_t mt_cksum_add(uint32_t sum, const uint8_t *p, uint32_t len);
uint16_t mt_cksum_fold(uint32_t sum);
/* Copy and checksum in one pass; same result as mt_cksum_add over src. src and dst may have
 * any alignment but must not overlap. */
uint32_t mt_cksum_copy(uint32_t sum, uint8_t *dst, const uint8_t *src, uint32_t len);
/* A chunk sum computed as if it started at an even stream offset, re-based to an odd one
 * (fold to 16 bits, swap the bytes). Add the result to the running sum. */
uint32_t mt_cksum_odd(uint32_t part);

/* Byte ring buffer. head = index of first used byte. Copies in and out use memcpy (the
 * Retro68 newlib memcpy moves long words when both pointers are even). */
typedef struct { uint8_t *base; uint32_t size, head, used; } mt_ring;
void mt_ring_init(mt_ring *r, uint8_t *mem, uint32_t size);
uint32_t mt_ring_free(const mt_ring *r);
uint32_t mt_ring_write(mt_ring *r, const uint8_t *p, uint32_t n);
uint32_t mt_ring_read(mt_ring *r, uint8_t *dst, uint32_t n);
/* Length of the contiguous run starting off bytes into the used data (0 if off >= used). */
uint32_t mt_ring_peek(const mt_ring *r, uint32_t off, const uint8_t **p);
void mt_ring_consume(mt_ring *r, uint32_t n);
/* Write at off bytes past the end of the used data without changing used (returns bytes that fit). */
uint32_t mt_ring_write_at(mt_ring *r, uint32_t off, const uint8_t *p, uint32_t n);

#endif
