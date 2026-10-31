/* mt_drv_mac.h - the Toolbox side of the .IPP driver (mt_drv_mac.c): memory, the unit table, the
 * Device Manager queue, entropy. Plain C interface: mt_drv.c includes mt_mactcp.h, which clashes
 * with the Toolbox headers, so every Toolbox call of the driver lives behind this header. */
#ifndef MT_DRV_MAC_H
#define MT_DRV_MAC_H

#include <stdint.h>

/* Unit the driver asks for: refnum -49, what MacTCP 2.1 gets (MACTCP-INTERFACE.md, obs). */
#define MT_DRV_UNIT 48

void   *mt_mac_alloc_sys(uint32_t n);          /* NewPtrSysClear; NULL on failure */
void    mt_mac_free(void *p);                  /* DisposePtr */
/* Nonzero if some installed driver is called `pname` (Pascal string, case-insensitive). */
int     mt_mac_unit_find(const uint8_t *pname);
/* Install `drvr` (a locked, never-moving DRVR block: header at offset 0) as a pointer-based driver
 * (ROM style: the Device Manager never locks, unlocks or moves it) in unit `want` or the first free
 * unit above it, growing the unit table when it is too short. 0 and *refnum, or an OSErr
 * (unitTblFullErr -29, memFullErr -108). */
int     mt_mac_unit_install(void *drvr, int16_t want, int16_t *refnum);
/* Make the system heap the current zone and the System file the current resource file, as an
 * application's environment would be (returns the previous zone; mt_mac_zone_restore puts both
 * back). The boot runs with them: .ENET0 is opened at INIT time, when the current resource file
 * is our own cdev file and the current zone may be the application zone (obs, Task 10: without
 * this, an address error inside the DaynaPORT driver right after its open). */
void   *mt_mac_zone_sys(void);
void    mt_mac_zone_restore(void *z);
/* Take a PB off the driver's request queue (the DCE's dCtlQHdr) and clear drvrActive. */
void    mt_mac_dequeue(void *pb, void *dce);
/* Entropy for mt_set_entropy: Ticks, Time, the mouse position, TickCount and the VIA timer. */
void    mt_mac_entropy(uint32_t *a, uint32_t *b);

#endif /* MT_DRV_MAC_H */
