/* mt_enet_mp.h - MoofTCP puts the MoofPort Ethernet driver into its "steadyack" poll settings (1.1d4, Task 30).
 *
 * MoofPort 1.1 boots with MoofPort 1.0's poll settings (better under MacTCP); MoofTCP's echo and terminal latency
 * are better with "steadyack". So when MoofTCP opens .ENET0 it asks the driver whether it is a MoofPort whose tune
 * layout it knows and, only then, applies steadyack through MoofPort's private Control call. Any other driver
 * (DaynaPORT, other versions, any error): nothing is changed and the open never fails because of this.
 *
 * Contract: MoofPort mp_drv.h MP_CTL_TUNE contract, MP_TUNE_VERSION 5 (MP_STATS_VERSION 8). The constants and
 * the layout below are copied from it (MoofTCP never includes MoofPort's headers):
 *  - Detect: Status csCode 0x4D50 on .ENET0, ePointer = a buffer, eBuffSize = its size (at least the driver's
 *    sizeof(MpStats), 176 for stats version 8; 256 here). noErr = MoofPort; it fills MpStats (big-endian).
 *    MpStats.version (uint16 at 0) >= 8 (fields are only ever appended) and MpStats.tune.version == 5.
 *  - Apply: Control csCode 0x4D54, ePointer = a 24-byte MpTune with every field set. The values last until the
 *    next driver reopen (close + open) or restart; opening an already-open driver keeps them.
 * The two driver calls go through mt_os.h (mt_os_enet_status / mt_os_enet_control).
 * Plain C, no Toolbox: host-tested (tests/test_enet_mp.c) against tests/pbfake.c. */
#ifndef MT_ENET_MP_H
#define MT_ENET_MP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MT_MP_STATUS_STATS  0x4D50   /* MP_STATUS_STATS ('MP') */
#define MT_MP_STATS_VERSION 8        /* MP_STATS_VERSION: the oldest MpStats layout read here */
#define MT_MP_CTL_TUNE      0x4D54   /* MP_CTL_TUNE ('MT') */
#define MT_MP_TUNE_VERSION  5        /* MP_TUNE_VERSION: the only MpTune layout sent */
#define MT_MP_STATS_BUF     256      /* Status buffer: generous (the driver only checks its own minimum) */

/* The layouts, for the offsets only (copied from mp_drv.h / MoofPort core/mp_dev.h; 68k natural alignment, the
 * same on the host). The bytes themselves are read and written big-endian (mt_enet_mp.c), not through these. */
typedef struct MtMpTune {
    uint16_t version;
    uint16_t burst_ms, hold_ms, idle_ms, step_ms, busy_ms;
    uint8_t  blind, verify, blind_tx, flags;
    uint16_t mid_ms, quiet_ms;
    uint8_t  early, pad, flags2, pad2;
} MtMpTune;
typedef struct MtMpStats {
    uint16_t version;
    int16_t  scsi_id;
    uint8_t  opened, mac[6], pad;
    uint32_t polls, busy, delivered, tx_now, tx_deferred, tx_failed, bus_err;
    int16_t  last_err, pad2;
    uint32_t rx[10];                 /* mp_rx_stats: 10 x uint32 */
    uint32_t tx[8];                  /* mp_tx_stats: 8 x uint32 */
    uint32_t unattached, runts;
    MtMpTune tune;
    uint32_t kicks, bursts, immediates, v_ok, v_bad, v_big, wd;
} MtMpStats;
#define MT_MP_TUNE_SIZE 24
#define MT_MP_STATS_TUNE_OFF 124     /* offsetof(MpStats, tune) */
_Static_assert(sizeof(MtMpTune) == MT_MP_TUNE_SIZE, "MpTune is 24 bytes (MP_TUNE_VERSION 5)");
_Static_assert(offsetof(MtMpStats, tune) == MT_MP_STATS_TUNE_OFF, "MpStats.tune offset (MP_STATS_VERSION 8)");
_Static_assert(sizeof(MtMpStats) == 176, "MpStats is 176 bytes (MP_STATS_VERSION 8)");
_Static_assert(MT_MP_STATS_BUF >= sizeof(MtMpStats), "Status buffer holds MpStats");

/* What mt_enet_mp_tune did (MtRes.mp_tune; MoofDiag's enet line). */
#define MT_MP_NONE     0             /* not MoofPort (Status refused: DaynaPORT, another driver) or not tried */
#define MT_MP_APPLIED  1             /* MoofPort, tune version 5: steadyack applied (Control noErr) */
#define MT_MP_VERSION  2             /* MoofPort, but a stats or tune layout this build does not know: left alone */
#define MT_MP_CTLFAIL  3             /* MoofPort, tune version 5, but the Control call failed (nothing applied) */

/* Ask the .ENET0 driver (refnum) whether it is a MoofPort with tune version 5 and, if so, apply steadyack. Task time
 * only: the driver open (mt_drv.c link_up, right after mt_enet_open succeeded: handlers attached), at every open
 * MoofTCP makes of .ENET0 (the watchdog's repair only detaches and re-attaches, so the tune survives it). Two
 * synchronous calls at most; never fails the caller (the result is only recorded, MtRes.mp_tune).
 * Uses a 256-byte stack buffer (task time, the application's stack). Returns MT_MP_*. */
uint8_t mt_enet_mp_tune(int16_t refnum);

#ifdef __cplusplus
}
#endif

#endif /* MT_ENET_MP_H */
