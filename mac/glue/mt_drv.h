/* mt_drv.h - the .IPP driver's resident state and entry points (mt_drv.c, mt_drv_stubs.s).
 * Plain C, no Toolbox types. */
#ifndef MT_DRV_H
#define MT_DRV_H

#include <stdint.h>
#include "mt.h"
#include "mt_gate.h"
#include "mt_enet.h"
#include "mt_timer.h"
#include "mt_drv_logic.h"
#include "mt_stackmark.h"
#include "mt_view.h"

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
    uint8_t        pad;
    mt_ifconfig    cfg;          /* configuration from the prefs (applied at the first open) */
    MtStackMark    stk;          /* interrupt-time stack depth (mt_stackmark.h); new fields go at the end */
    uint8_t        cfg_flags;    /* MCFG flags byte (MT_CFG_FLAG_LOG ...) */
    uint8_t        pad2;
    /* Plan 4 (DHCP), appended: version stays 1 */
    mt_ip4         dhcp_hint;    /* DHCP mode: the prefs address, sent as the requested address */
    MtStatusView   view;         /* status for the panel / MoofDiag (mt_view.h); DCE dCtlStorage -> here */
} MtRes;

/* mt_drv_boot results (the INIT draws its icon only for MT_BOOT_OK) */
#define MT_BOOT_OK            0
#define MT_BOOT_UNCONFIGURED  1   /* installed, no address: OpenDriver(".IPP") gives -23004 */
#define MT_BOOT_AGAIN         2   /* already booted (a second call) */
#define MT_BOOT_DHCP          3   /* installed in DHCP mode: the address comes at the first open (icon drawn) */
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
 * offset 32) receives MT_STATUS_VERSION (uint16), for the diagnostics app mac/moofdiag and the
 * control panel. A client checks the version (the second value, or MtRes.version, the block's first
 * word) BEFORE reading anything else; any layout change of MtRes that moves or removes a field bumps
 * it (new fields appended at the end keep the version). Every other Status call gets statusErr (-18).
 * Version 1 = Plan 3 Task 6 (adds version first, cfg_flags last; Task 1 stack marks, Task 2 stall
 * counter, Task 3 loopback counters and Task 5 watchdog counters are all kept). */
#define MT_DRV_STATUS_RES 0x4D54
#define MT_STATUS_VERSION 1

/* DHCP mode: the first open waits this long for the lease (DHCP-DESIGN.md section 4). */
#define MT_DRV_DHCP_WAIT_TICKS 600u    /* 10 s */

int16_t mt_drv_open(void *pb, void *dce);
int16_t mt_drv_prime(void *pb, void *dce);
int16_t mt_drv_control(void *pb, void *dce);
int16_t mt_drv_status(void *pb, void *dce);
int16_t mt_drv_close(void *pb, void *dce);

#endif /* MT_DRV_H */
