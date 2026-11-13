/* test_enet_mp.c - MoofTCP switches MoofPort to "steadyack" at the .ENET0 open (mac/glue/mt_enet_mp.c, Task 30)
 * against the fake OS: the driver's Status / Control answers are scripted (tests/pbfake.c). */
#include "t.h"
#include "pbfake.h"
#include "mt_enet.h"
#include "mt_enet_mp.h"

#define REF (-48)

/* steadyack, the MP_CTL_TUNE contract's MpTune (MP_TUNE_VERSION 5), big-endian as the 68k driver reads it */
static const uint8_t STEADYACK[24] = {
    0x00, 0x05,                  /* version 5 */
    0x00, 0x10,                  /* burst_ms 16 */
    0x00, 0x3C,                  /* hold_ms 60 */
    0x00, 0x10,                  /* idle_ms 16 */
    0x00, 0x00,                  /* step_ms 0 */
    0x00, 0x02,                  /* busy_ms 2 */
    0x01,                        /* blind 1 */
    0x00,                        /* verify 0 */
    0x01,                        /* blind_tx 1 */
    0x1C,                        /* flags NOYIELD 0x10 | NOCHASE 0x08 | ACK2 0x04 */
    0x00, 0x10,                  /* mid_ms 16 */
    0x07, 0xD0,                  /* quiet_ms 2000 */
    0x02,                        /* early MP_EARLY_OFF */
    0x00,                        /* pad */
    0x01,                        /* flags2 NOPAUSE */
    0x00                         /* pad2 */
};

/* A MoofPort answering Status 0x4D50: MpStats (176 bytes) with the given stats and tune versions, other fields
 * nonzero so a misplaced read shows. */
static void moofport(uint16_t stats_v, uint16_t tune_v)
{
    int i;
    for (i = 0; i < 176; i++) pbf_enet_status_data[i] = (uint8_t)(0x40 + i);
    pbf_enet_status_data[0] = (uint8_t)(stats_v >> 8);
    pbf_enet_status_data[1] = (uint8_t)stats_v;
    pbf_enet_status_data[MT_MP_STATS_TUNE_OFF] = (uint8_t)(tune_v >> 8);
    pbf_enet_status_data[MT_MP_STATS_TUNE_OFF + 1] = (uint8_t)tune_v;
    pbf_enet_status_len = 176;
    pbf_enet_status_result = 0;
}

static void setup(void)
{
    pbf_reset();
}

/* (a) MoofPort 1.1 (stats 8, tune 5): one Status 0x4D50 with a 256-byte buffer, then exactly one Control 0x4D54
 * carrying the 24 steadyack bytes */
static void moofport_v5_gets_steadyack(void)
{
    setup();
    moofport(8, 5);
    T_EQ(mt_enet_mp_tune(REF), MT_MP_APPLIED);
    T_EQ(pbf_enet_status_calls, 1);
    T_EQ(pbf_enet_status_cs, 0x4D50);
    T_EQ(pbf_enet_status_size, 256);
    T_EQ(pbf_enet_status_ref, REF);
    T_EQ(pbf_enet_ctl_calls, 1);
    T_EQ(pbf_enet_ctl_cs, 0x4D54);
    T_EQ(pbf_enet_ctl_ref, REF);
    T_MEMEQ(pbf_enet_ctl_bytes, STEADYACK, 24);
    T_EQ(pbf_bad, 0);
}

/* (b) DaynaPORT: the private Status is refused (statusErr, nothing filled): no Control, nothing to report */
static void daynaport_status_error_no_control(void)
{
    static const int16_t errs[] = { -18, -17, -1, -36, -28, 1 };
    unsigned i;
    for (i = 0; i < sizeof errs / sizeof errs[0]; i++) {
        setup();
        pbf_enet_status_result = errs[i];
        T_EQ(mt_enet_mp_tune(REF), MT_MP_NONE);
        T_EQ(pbf_enet_status_calls, 1);
        T_EQ(pbf_enet_ctl_calls, 0);
    }
}

/* (c) a MoofPort with another tune layout (6, 4, 0) or an older stats layout (7): left alone */
static void moofport_other_versions_left_alone(void)
{
    static const uint16_t v[][2] = { { 8, 6 }, { 8, 4 }, { 8, 0 }, { 7, 5 }, { 1, 5 }, { 8, 0x0500 } };
    unsigned i;
    for (i = 0; i < sizeof v / sizeof v[0]; i++) {
        setup();
        moofport(v[i][0], v[i][1]);
        T_EQ(mt_enet_mp_tune(REF), MT_MP_VERSION);
        T_EQ(pbf_enet_ctl_calls, 0);
    }
}

/* fields are only appended to MpStats: a newer stats layout with tune version 5 still gets the tune */
static void newer_stats_layout_with_tune_v5_applied(void)
{
    setup();
    moofport(9, 5);
    T_EQ(mt_enet_mp_tune(REF), MT_MP_APPLIED);
    T_EQ(pbf_enet_ctl_calls, 1);
    T_MEMEQ(pbf_enet_ctl_bytes, STEADYACK, 24);
}

/* (b') the DaynaPORT driver in Snow answers the unknown Status with noErr and fills nothing (1.1d4 mem-check): the
 * buffer was cleared, so it reads stats version 0: not MoofPort, no Control */
static void noerr_without_data_is_not_moofport(void)
{
    setup();
    pbf_enet_status_result = 0;
    pbf_enet_status_len = 0;
    T_EQ(mt_enet_mp_tune(REF), MT_MP_NONE);
    T_EQ(pbf_enet_status_calls, 1);
    T_EQ(pbf_enet_ctl_calls, 0);
}

/* (d) the Control call fails: reported, nothing else (the caller's open goes on) */
static void control_failure_reported(void)
{
    static const int16_t errs[] = { -17, -18, -28, -1 };
    unsigned i;
    for (i = 0; i < sizeof errs / sizeof errs[0]; i++) {
        setup();
        moofport(8, 5);
        pbf_enet_ctl_result = errs[i];
        T_EQ(mt_enet_mp_tune(REF), MT_MP_CTLFAIL);
        T_EQ(pbf_enet_ctl_calls, 1);
    }
}

/* (e) every open applies it again (a close and open of MoofPort brings back its boot tune): no state is kept */
static void each_open_applies_again(void)
{
    setup();
    moofport(8, 5);
    T_EQ(mt_enet_mp_tune(REF), MT_MP_APPLIED);
    T_EQ(mt_enet_mp_tune(REF), MT_MP_APPLIED);
    T_EQ(pbf_enet_status_calls, 2);
    T_EQ(pbf_enet_ctl_calls, 2);
    T_MEMEQ(pbf_enet_ctl_bytes, STEADYACK, 24);
}

/* (f) the watchdog's repair (KillIO, then detach/attach, no close/open) never sends the tune calls: the tune
 * survives it in the driver */
static void reattach_sends_no_tune_calls(void)
{
    static MtEnet e;
    unsigned i;
    setup();
    memset(&e, 0, sizeof e);
    e.refnum = REF;
    e.opened = 1;
    e.gate = (MtGate *)pbf_alloc(sizeof(MtGate));
    moofport(8, 5);
    e.tx[0].started = 1;                         /* one write stuck in the driver */
    e.tx[0].pb.ioResult = 1;
    e.tx_started = e.tx_live = 1;
    for (i = 0; i < 4; i++) mt_enet_watchdog(&e, i * MT_ENET_TX_STUCK_MS + 1u), mt_enet_service(&e, i * MT_ENET_TX_STUCK_MS + 1u);
    pbf_reattach_result = -1;                    /* first attempt fails, then succeeds */
    mt_enet_service(&e, 5u * MT_ENET_TX_STUCK_MS);
    pbf_reattach_result = 0;
    mt_enet_service(&e, 10u * MT_ENET_TX_STUCK_MS);
    T_CHECK(pbf_kill_calls >= 1);
    T_CHECK(pbf_reattach_calls >= 1);
    T_EQ(pbf_enet_status_calls, 0);
    T_EQ(pbf_enet_ctl_calls, 0);
}

T_MAIN(moofport_v5_gets_steadyack, daynaport_status_error_no_control, moofport_other_versions_left_alone,
       newer_stats_layout_with_tune_v5_applied, noerr_without_data_is_not_moofport, control_failure_reported,
       each_open_applies_again, reattach_sends_no_tune_calls)
