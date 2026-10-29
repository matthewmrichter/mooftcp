/* mt_enet_logic.h - the pure parts of the Ethernet / clock glue (Task 9), header-only so the
 * host tests (tests/test_enet_logic.c) run them under ASan/UBSan. No Toolbox, no globals. */
#ifndef MT_ENET_LOGIC_H
#define MT_ENET_LOGIC_H

#include <stdint.h>
#include <string.h>

#define MT_ENET_MINFRAME 60u        /* smallest frame the driver sends without the FCS */
#define MT_ENET_MAXFRAME 1514u

/* Length to hand to EWrite: IP frames come out of the core unpadded (a pure ACK is 54 bytes). */
static inline uint16_t mt_enet_padded_len(uint16_t len)
{
    return len < MT_ENET_MINFRAME ? (uint16_t)MT_ENET_MINFRAME : len;
}

/* Zero the pad bytes [len, 60) of buf (buf must hold 60 bytes); returns the padded length. */
static inline uint16_t mt_enet_pad(uint8_t *buf, uint16_t len)
{
    if (len < MT_ENET_MINFRAME) memset(buf + len, 0, MT_ENET_MINFRAME - len);
    return mt_enet_padded_len(len);
}

/* A frame is worth sending: at least the Ethernet header and no more than a full frame. */
static inline int mt_enet_len_ok(uint16_t len)
{
    return len >= 14u && len <= MT_ENET_MAXFRAME;
}

/* Milliseconds from the Mac tick count (60.1474 Hz: 16.625 ms per tick, exact to 0.004%).
 * ms = 16 t + 5 t / 8 without ever forming a product that overflows before t itself wraps, so
 * the result is monotonic modulo 2^32 up to the wrap of the 32-bit tick count (2.3 years of uptime;
 * the single step across that wrap is wrong by a fraction of 2^32 ms, which the core's wrap-safe
 * compares absorb as one spurious jump). */
static inline uint32_t mt_ms_from_ticks(uint32_t t)
{
    return t * 16u + (t >> 3) * 5u + (((t & 7u) * 5u) >> 3);
}

#endif /* MT_ENET_LOGIC_H */
