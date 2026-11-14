/* test_drv_status.c - the driver's private Status (csCode 0x4D54, mt_drv_status -> mt_drv_status_res in mt_drv.h):
 * block address, layout version and revision for MoofDiag; every other csCode statusErr. */
#include <stddef.h>
#include "t.h"
#include "mt_drv.h"

/* a Status parameter block (CntrlParam: 50 bytes, csCode at 26, csParam at 28), cleared as a reader clears it */
static uint8_t PB[50];

static void status_pb(int16_t cs)
{
    memset(PB, 0, sizeof PB);
    memcpy(PB + 26, &cs, 2);
}

static int16_t rd16(int off) { int16_t v; memcpy(&v, PB + off, 2); return v; }
static uint32_t rd32(int off) { uint32_t v; memcpy(&v, PB + off, 4); return v; }

static void private_status_returns_block_version_and_revision(void)
{
    static MtRes res;
    status_pb(MT_DRV_STATUS_RES);
    T_EQ(MT_DRV_STATUS_RES, 0x4D54);
    T_EQ(mt_drv_status_res(PB, rd16(26), &res), 0);
    T_EQ(rd32(28), (uint32_t)(uintptr_t)&res);
    T_EQ(rd16(32), MT_STATUS_VERSION);
    T_EQ(rd16(32), 1);
    T_EQ(rd16(34), MT_STATUS_REV);
    T_EQ(rd16(34), 2);                       /* rev 2: .mp_tune (1.1d4) */
    T_EQ(rd16(26), MT_DRV_STATUS_RES);       /* the header is untouched */
    T_EQ(PB[0] | PB[24] | PB[25] | PB[36] | PB[49], 0);
}

static void other_status_calls_get_statuserr(void)
{
    static MtRes res;
    static const int16_t cs[] = { 0, 1, 8, 0x4D50, 0x4D55, -1 };
    unsigned i;
    uint8_t zero[50];
    memset(zero, 0, sizeof zero);
    for (i = 0; i < sizeof cs / sizeof cs[0]; i++) {
        status_pb(cs[i]);
        T_EQ(mt_drv_status_res(PB, rd16(26), &res), -18);
        T_MEMEQ(PB + 28, zero, sizeof PB - 28);   /* csParam left alone */
    }
    status_pb(MT_DRV_STATUS_RES);            /* not booted: no block */
    T_EQ(mt_drv_status_res(PB, rd16(26), NULL), -18);
    T_MEMEQ(PB + 28, zero, sizeof PB - 28);
}

/* the revision's appended fields stay after every field of the earlier revisions (version 1 readers keep reading) */
static void rev2_field_is_appended(void)
{
    T_CHECK(offsetof(MtRes, mp_tune) > offsetof(MtRes, n_accrun));
    T_CHECK(offsetof(MtRes, n_accrun) > offsetof(MtRes, shot));
    T_CHECK(offsetof(MtRes, shot) > offsetof(MtRes, view));
#ifdef MT_TUNE
    T_CHECK(offsetof(MtRes, pump) > offsetof(MtRes, mp_tune));
#endif
}

T_MAIN(private_status_returns_block_version_and_revision, other_status_calls_get_statuserr, rev2_field_is_appended)
