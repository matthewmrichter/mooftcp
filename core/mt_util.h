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

#endif
