/* mt_enet_mp.c - put a MoofPort .ENET0 driver into "steadyack" at the open (see mt_enet_mp.h). Plain C, no Toolbox:
 * the two driver calls go through mt_os.h; host-tested (tests/test_enet_mp.c). */
#include "mt_enet_mp.h"
#include "mt_os.h"

/* steadyack: the MP_CTL_TUNE contract's MpTune, every field, big-endian (the driver's layout, MP_TUNE_VERSION 5) */
static const uint8_t steadyack[MT_MP_TUNE_SIZE] = {
    0x00, MT_MP_TUNE_VERSION,    /* version */
    0x00, 16,                    /* burst_ms */
    0x00, 60,                    /* hold_ms */
    0x00, 16,                    /* idle_ms */
    0x00, 0,                     /* step_ms */
    0x00, 2,                     /* busy_ms */
    1,                           /* blind */
    0,                           /* verify */
    1,                           /* blind_tx */
    0x10 | 0x08 | 0x04,          /* flags: MP_TUNE_NOYIELD | MP_TUNE_NOCHASE | MP_TUNE_ACK2 */
    0x00, 16,                    /* mid_ms */
    2000 >> 8, 2000 & 0xFF,      /* quiet_ms */
    2,                           /* early: MP_EARLY_OFF */
    0,                           /* pad */
    0x01,                        /* flags2: MP_TUNE2_NOPAUSE (MP_TUNE2_STEADY off) */
    0                            /* pad2 */
};

/* Runs only at the driver open (mt_enet_open): cold, built for size like the other open-time code (1.1d3 pattern).
 * The 256-byte buffer is on the opening application's stack at task time, never on an interrupt-time path. */
__attribute__((cold)) uint8_t mt_enet_mp_tune(int16_t refnum)
{
    uint8_t st[MT_MP_STATS_BUF];
    unsigned i;
    for (i = 0; i < sizeof st; i++) st[i] = 0;   /* a driver that says noErr but fills nothing reads as version 0 */
    if (mt_os_enet_status(refnum, MT_MP_STATUS_STATS, st, (int16_t)sizeof st) != 0)
        return MT_MP_NONE;                       /* DaynaPORT and other drivers: unknown Status csCode */
    if (((unsigned)st[0] << 8 | st[1]) < MT_MP_STATS_VERSION ||
        ((unsigned)st[MT_MP_STATS_TUNE_OFF] << 8 | st[MT_MP_STATS_TUNE_OFF + 1]) != MT_MP_TUNE_VERSION)
        return MT_MP_VERSION;                    /* a MoofPort whose tune layout this build does not know */
    return mt_os_enet_control(refnum, MT_MP_CTL_TUNE, steadyack) == 0 ? MT_MP_APPLIED : MT_MP_CTLFAIL;
}
