/* mt_view.h - the driver's status as the control panel and MoofDiag read it (Plan 4, DHCP).
 *
 * One MtStatusView lives in the driver's resident block (MtRes.view). The driver puts its address
 * in its DCE's dCtlStorage (offset 20) at boot, so a reader that found the ".ipp" unit in the unit
 * table (mt_units.h) reaches it without opening or calling the driver (panel ruling P3). A reader
 * checks magic and version first; a driver that predates this header leaves dCtlStorage 0.
 *
 * Writers: the boot (mode, addr of a manual configuration), link_up (linked), and the DHCP event
 * callback (mt_pb_dhcp_event, at interrupt time possibly): state, addr, lease_s, t_bound. Readers
 * run at task time without any lock: every field is written by one store, a torn view is at worst
 * one stale field for one redraw. Plain C, no Toolbox types (included by the host tests). */
#ifndef MT_VIEW_H
#define MT_VIEW_H

#include <stdint.h>

#define MT_VIEW_MAGIC   0x4D76u     /* 'Mv' */
#define MT_VIEW_VERSION 1u
#define MT_VIEW_DCE_STORAGE 20      /* dCtlStorage offset in the DCE */

typedef struct MtStatusView {
    uint16_t magic;                 /* MT_VIEW_MAGIC */
    uint16_t version;               /* MT_VIEW_VERSION; new fields go at the end */
    uint8_t  mode;                  /* 0 manual, 1 DHCP (MCFG mode word) */
    volatile uint8_t state;         /* DHCP: mt_dhcp_state (0 = not started: no application yet) */
    volatile uint8_t linked;        /* 1 once the first OpenDriver brought the link up */
    uint8_t  pad;
    volatile uint32_t addr;         /* address in use (manual: the configured one), 0 = none */
    volatile uint32_t lease_s;      /* DHCP: granted lease in seconds (0xFFFFFFFF infinite) */
    volatile uint32_t t_bound;      /* DHCP: TickCount when the lease (or its renewal) began */
} MtStatusView;

#endif /* MT_VIEW_H */
