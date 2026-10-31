/* mt_units.h - find an installed ".ipp" driver in the unit table, without opening anything
 * (Plan 3 Task 8, Review Focus 2 and ruling P3).
 *
 * Used by the INIT (mac/init/mt_init.c: back off when a .ipp driver is already installed, so a
 * copy of MoofTCP next to the real MacTCP never replaces MacTCP's driver) and by the control panel
 * (mac/cdev/cdev_main.c: the status line). Same walk as MoofDiag's unit listing: UTableBase
 * (0x11C) holds UnitNtryCnt (0x1D2) DCE handles; a DCE's dCtlDriver (offset 0) is a pointer, or a
 * handle when dCtlFlags (offset 4) has dRAMBased (0x40); the driver's name is the Pascal string at
 * offset 18. Every address is masked to 24 bits (System 6 on a 68000: 24-bit mode). Header-only and
 * free of absolute data references, so the position-independent cdev code can include it. */
#ifndef MT_UNITS_H
#define MT_UNITS_H

#include <stdint.h>

#define MT_UNITS_S24(p) ((const uint8_t *)((uint32_t)(uintptr_t)(p) & 0x00FFFFFFu))

/* The driver header of the first unit whose driver is named ".ipp" (any case), or NULL. */
static inline const uint8_t *mt_units_find_ipp(void) {
    uint8_t **const *tbl = *(uint8_t **const *volatile *)0x11C;
    int16_t n = *(volatile int16_t *)0x1D2, i;
    if (!tbl) return NULL;
    tbl = (uint8_t **const *)MT_UNITS_S24(tbl);
    for (i = 0; i < n; i++) {
        uint8_t **h = tbl[i];
        const uint8_t *dce, *drv;
        if (!h) continue;
        h = (uint8_t **)MT_UNITS_S24(h);
        if (!*h) continue;
        dce = MT_UNITS_S24(*h);
        drv = *(const uint8_t *const *)dce;
        if (!drv) continue;
        drv = MT_UNITS_S24(drv);
        if (dce[5] & 0x40) {                       /* dRAMBased: dCtlDriver is a handle */
            drv = *(const uint8_t *const *)drv;
            if (!drv) continue;                    /* purged */
            drv = MT_UNITS_S24(drv);
        }
        if (drv[18] == 4 && drv[19] == '.' && (drv[20] | 0x20) == 'i' && (drv[21] | 0x20) == 'p' &&
            (drv[22] | 0x20) == 'p')
            return drv;
    }
    return NULL;
}

/* 1 if drv (from mt_units_find_ipp) is MoofTCP's driver: its open entry is at offset 0x18 and
 * starts with MoofTCP's stub "moveq #0,d0; bra.s 48" (mac/moof/moof.r). MacTCP's open entry is at
 * 0x32. */
static inline int mt_units_is_moof(const uint8_t *drv) {
    return drv && drv[8] == 0x00 && drv[9] == 0x18 && drv[24] == 0x70 && drv[25] == 0x00 &&
           drv[26] == 0x60 && drv[27] == 0x14;
}

#endif
