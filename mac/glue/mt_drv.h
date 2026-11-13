/* mt_drv.h - the .IPP driver's resident state and entry points (mt_drv.c, mt_drv_stubs.s).
 * Plain C, no Toolbox types. */
#ifndef MT_DRV_H
#define MT_DRV_H

#include <stdint.h>
#include <string.h>
#include "mt.h"
#include "mt_gate.h"
#include "mt_enet.h"
#include "mt_enet_mp.h"
#include "mt_timer.h"
#include "mt_drv_logic.h"
#include "mt_stackmark.h"
#include "mt_view.h"
#ifdef MT_TRACE
#include "mt_trace.h"
#endif

struct MtDrv;

/* Everything the driver keeps, in ONE non-relocatable system-heap block allocated at boot; the
 * MtDrv and the core arena follow it (8-aligned offset) in the same block. */
typedef struct MtRes {
    uint16_t       version;      /* MT_STATUS_VERSION; FIRST, so a reader can check it before trusting the rest */
    MtGate         gate;         /* input ring + core ownership */
    MtEnet         enet;         /* .ENET0, transmit slots */
    MtTimer        timer;        /* Time Manager task */
    MtEntry        entry;        /* Control entry: queued / refused PBs, counters */
    struct MtDrv  *d;
    void          *raw;          /* the block as allocated (2-aligned) */
    uint32_t       size;         /* its size */
    int16_t        refnum;       /* -49 normally */
    int16_t        cfg_result;   /* mt_cfg_load result */
    int16_t        enet_err;     /* unit install error at boot / mt_enet_open error at open */
    uint8_t        mode;         /* MCFG mode slot */
    uint8_t        armed;        /* 1.1d2 (was a zero pad byte): an early timer tick armed the timer; the gate's
                                  * after callback that follows re-arms only if its drain ran a PB (mt_drv.c) */
    mt_ifconfig    cfg;          /* configuration from the prefs (applied at the first open) */
    MtStackMark    stk;          /* interrupt-time stack depth (mt_stackmark.h); new fields go at the end */
    uint8_t        cfg_flags;    /* MCFG flags byte (MT_CFG_FLAG_LOG ...) */
    uint8_t        variant;      /* build variant, MT_RES_VARIANT (was a zero pad byte: 0 = full) */
    /* Plan 4 (DHCP), appended: version stays 1. MoofTCP Lite keeps the same layout (dhcp_hint stays 0,
     * view.mode stays 0), so readers of the view need no variant check. */
    mt_ip4         dhcp_hint;    /* DHCP mode: the prefs address, sent as the requested address */
    MtStatusView   view;         /* status for the panel / MoofDiag (mt_view.h); DCE dCtlStorage -> here */
    /* 1.1, appended (MT_STATUS_REV 1; readers check the revision first, see below) */
    MtTimerShot    shot;         /* shot.due is the absolute time (mt_os_ms) the timer's pending shot is for, valid
                                  * while timer.armed; shot.lost / lost_at / lost_max count the lost shots
                                  * mt_timer_arm_deadline found and how late (MoofDiag's timer: line) */
    uint32_t       n_accrun;     /* accRun heartbeats (SystemTask, DRVR dNeedTime; mt_entry_accrun) */
    /* 1.1d4, appended (MT_STATUS_REV 2) */
    uint8_t        mp_tune;      /* what the last .ENET0 open did to a MoofPort driver's poll settings: MT_MP_*
                                  * (mt_enet_mp.h: 0 not MoofPort, 1 steadyack applied, 2 unknown version, 3 failed) */
    uint8_t        pad_rev2;
#ifdef MT_TUNE
    MtEnetPump     pump;         /* perf builds: the transmit pump (mt_enet.h), lever txpump; appended so every field
                                  * above keeps its place for readers built without MT_TUNE */
#endif
#ifdef MT_TRACE
    MtTrace        trace;        /* perf builds with MT_TRACE (task 18): the echo-path trace's ring and clock (mt_trace.h),
                                  * read through Control MT_CS_TRACE; last, after the pump */
#endif
} MtRes;

/* MtRes.variant: which build made the driver. The core's mt_stack differs between the two (Lite has
 * no DHCP section pointer), so a reader that walks the stack (MoofDiag) must be the same variant. */
#define MT_RES_VARIANT_FULL 0
#define MT_RES_VARIANT_LITE 1      /* MT_NO_DHCP: MoofTCP Lite */
#ifdef MT_NO_DHCP
#define MT_RES_VARIANT MT_RES_VARIANT_LITE
#else
#define MT_RES_VARIANT MT_RES_VARIANT_FULL
#endif

/* mt_drv_boot results (the INIT draws its icon for MT_BOOT_OK and MT_BOOT_DHCP) */
#define MT_BOOT_OK            0
#define MT_BOOT_UNCONFIGURED  1   /* installed, no address: OpenDriver(".IPP") gives -23004 */
#define MT_BOOT_AGAIN         2   /* already booted (a second call) */
#define MT_BOOT_DHCP          3   /* installed in DHCP mode: the address comes at the first open (icon drawn;
                                   * never in Lite, which treats a DHCP-mode prefs file as manual) */
#define MT_BOOT_TAKEN       (-1)  /* a driver named .ipp is already installed (MacTCP) */
#define MT_BOOT_NOMEM       (-2)
#define MT_BOOT_NOUNIT      (-3)  /* the unit table install failed */

/* Called by the INIT through the boot stub (DRVR offset 44) with the DRVR block (locked, in the
 * system heap, never moved afterwards). Relocates the code, allocates, reads the prefs, installs
 * (.ENET0 is opened by the first OpenDriver, see mt_drv.c link_up). On
 * a negative result nothing is left installed and the caller disposes the block. */
long    mt_drv_boot(void *drvr);
MtRes  *mt_drv_res(void);        /* the resident block (NULL before boot) */

/* Private Status csCode ('MT'): csParam (PB offset 28) receives the MtRes pointer, csParam+4 (PB
 * offset 32) receives MT_STATUS_VERSION (uint16), csParam+6 (PB offset 34) MT_STATUS_REV (uint16), for
 * the diagnostics app mac/moofdiag and the control panel. A client checks the version (the second
 * value, or MtRes.version, the block's first word) BEFORE reading anything else; any layout change of
 * MtRes that moves or removes a field bumps it (new fields appended at the end keep the version, so
 * every reader of version 1, 1.0's MoofDiag included, keeps reading the fields it knows). Every other
 * Status call gets statusErr (-18).
 * Version 1 = Plan 3 Task 6 (adds version first, cfg_flags last; Task 1 stack marks, Task 2 stall
 * counter, Task 3 loopback counters and Task 5 watchdog counters are all kept).
 * The revision says which appended fields a version-1 block has; a reader clears offset 34 before the
 * call (a driver older than the revision word leaves it alone) and reads an appended field only when
 * the revision says it is there:
 *   rev 0: MtRes ends at .view (1.0; also the 1.1 development drivers 1.1d1-1.1d3, whose appended
 *          fields had another layout)
 *   rev 1: .shot (due, lost, lost_at, lost_max) and .n_accrun (1.1)
 *   rev 2: .mp_tune (1.1d4: MoofTCP switches a MoofPort .ENET0 to steadyack at the open, mt_enet_mp.h)
 * Opening failed (no address): the reader finds the block through the status view instead and has no
 * revision; it reads rev 0. */
#define MT_DRV_STATUS_RES 0x4D54
#define MT_STATUS_VERSION 1
#define MT_STATUS_REV     2

/* mt_drv_status's private call (above), plain C so the host tests run it: a Status PB (CntrlParam) with csCode
 * MT_DRV_STATUS_RES and a booted block gets the block address at PB offset 28 and the version and revision at 32
 * and 34 (0); anything else gets statusErr (-18) and the PB is left alone. */
static inline int16_t mt_drv_status_res(void *pb, const void *res) {
    uint8_t *b = (uint8_t *)pb;
    int16_t cs;
    memcpy(&cs, b + 26, 2);
    if (cs == MT_DRV_STATUS_RES && res) {
        uint32_t p = (uint32_t)(uintptr_t)res;
        uint16_t v[2] = { MT_STATUS_VERSION, MT_STATUS_REV };
        memcpy(b + 28, &p, 4);
        memcpy(b + 32, v, 4);
        return 0;
    }
    return -18;                                  /* statusErr: .IPP answers only Control */
}

#ifndef MT_NO_DHCP
/* DHCP mode: the first open waits this long for the lease (DHCP-DESIGN.md section 4). */
#define MT_DRV_DHCP_WAIT_TICKS 600u    /* 10 s */
#endif

int16_t mt_drv_open(void *pb, void *dce);
int16_t mt_drv_prime(void *pb, void *dce);
int16_t mt_drv_control(void *pb, void *dce);
int16_t mt_drv_status(void *pb, void *dce);
int16_t mt_drv_close(void *pb, void *dce);

#endif /* MT_DRV_H */
