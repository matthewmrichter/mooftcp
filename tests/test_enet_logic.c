/* test_enet_logic.c - the pure logic of the Ethernet/clock/gate glue (mac/glue/mt_enet_logic.h,
 * frame padding, tick-to-ms conversion. */
#include "t.h"
#include "mt_enet_logic.h"

static void pad_lengths(void)
{
    uint8_t b[64];
    int i;
    T_EQ(mt_enet_padded_len(0), 60);
    T_EQ(mt_enet_padded_len(54), 60);
    T_EQ(mt_enet_padded_len(59), 60);
    T_EQ(mt_enet_padded_len(60), 60);
    T_EQ(mt_enet_padded_len(61), 61);
    T_EQ(mt_enet_padded_len(1514), 1514);
    memset(b, 0xAA, sizeof b);
    T_EQ(mt_enet_pad(b, 54), 60);
    for (i = 0; i < 54; i++) T_EQ(b[i], 0xAA);
    for (i = 54; i < 60; i++) T_EQ(b[i], 0);
    T_EQ(b[60], 0xAA);                 /* nothing past 60 touched */
    memset(b, 0xAA, sizeof b);
    T_EQ(mt_enet_pad(b, 60), 60);
    T_EQ(b[59], 0xAA);
    T_CHECK(!mt_enet_len_ok(13));
    T_CHECK(mt_enet_len_ok(14));
    T_CHECK(mt_enet_len_ok(1514));
    T_CHECK(!mt_enet_len_ok(1515));
}

static void ms_from_ticks(void)
{
    uint32_t t, prev = 0, d;
    T_EQ(mt_ms_from_ticks(0), 0);
    T_EQ(mt_ms_from_ticks(1), 16);       /* 16.625 truncated */
    T_EQ(mt_ms_from_ticks(8), 133);      /* exactly 8 * 16.625 */
    T_EQ(mt_ms_from_ticks(60), 997);     /* 60 ticks = 997.5 ms */
    T_EQ(mt_ms_from_ticks(3600), 59850); /* a minute of 60 Hz ticks */
    /* the step per tick is 16 or 17 ms right up to the last tick before the 32-bit tick count
     * wraps (2.3 years of uptime: that one step is not 16/17, documented in mt_enet_logic.h) */
    for (t = 0xFFFFFF00u; t != 0xFFFFFFFFu; t++) {
        d = mt_ms_from_ticks(t + 1) - mt_ms_from_ticks(t);
        T_CHECK(d == 16 || d == 17);
    }
    for (t = 1u << 28; t < (1u << 28) + 4096; t++) {
        d = mt_ms_from_ticks(t) - prev;
        if (t != (1u << 28)) T_CHECK(d == 16 || d == 17);
        prev = mt_ms_from_ticks(t);
    }
    /* the sum of the steps matches the real rate to within one ms per 8 ticks over 10 days */
    T_EQ(mt_ms_from_ticks(8u * 1000000u) , 133000000u);
}

T_MAIN(pad_lengths, ms_from_ticks)
