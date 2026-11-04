/* mt_units.h - find an installed ".ipp" driver in the unit table, without opening anything
 * (Plan 3 Task 8, Review Focus 2 and ruling P3).
 *
 * Used by the INIT (mac/init/mt_init.c: back off when a .ipp driver is already installed, so a
 * copy of MoofTCP next to the real MacTCP never replaces MacTCP's driver) and by the control panel
 * (mac/cdev/cdev_main.c: the status line). Same walk as MoofDiag's unit listing: UTableBase
 * (0x11C) holds UnitNtryCnt (0x1D2) DCE handles; a DCE's dCtlDriver (offset 0) is a pointer, or a
 * handle when dCtlFlags (the word after it, low byte) has dRAMBased (0x40); the driver's name is
 * the Pascal string at offset 18. On the Mac every address is masked to 24 bits (System 6 on a
 * 68000). Header-only and free of absolute data references, so the position-independent cdev code
 * can include it.
 *
 * mt_units_find_ipp_in is the walk with the table and the address mask passed in, so the host test
 * (tests/test_cdev_logic.c) can run it on fake DCEs built with native pointers: the flags word sits
 * right after dCtlDriver, at offset sizeof(void *) (4 on the 68k, as in the real DCE). */
#ifndef MT_UNITS_H
#define MT_UNITS_H

#include <stddef.h>
#include <stdint.h>
#include "mt_view.h"

#define MT_UNITS_MASK24 ((uintptr_t)0x00FFFFFFu)
#define MT_UNITS_PTR(p, mask) ((const uint8_t *)((uintptr_t)(p) & (mask)))
/* dCtlStorage: offset 20 in the real DCE (after dCtlDriver 4, flags 2, queue header 10, position
 * 4); host fakes with 8-byte pointers put it at 24. */
#define MT_UNITS_DCE_STORAGE ((int)(sizeof(void *) == 4 ? 20 : 24))

/* As mt_units_find_ipp_in, also giving the unit's DCE (NULL if none). */
static inline const uint8_t *mt_units_find_ipp_dce_in(uint8_t **const *tbl, int n, uintptr_t mask,
                                                      const uint8_t **dce_out) {
    int i;
    if (dce_out) *dce_out = NULL;
    if (!tbl) return NULL;
    tbl = (uint8_t **const *)MT_UNITS_PTR(tbl, mask);
    for (i = 0; i < n; i++) {
        uint8_t **h = tbl[i];
        const uint8_t *dce, *drv;
        if (!h) continue;
        h = (uint8_t **)MT_UNITS_PTR(h, mask);
        if (!*h) continue;
        dce = MT_UNITS_PTR(*h, mask);
        drv = *(const uint8_t *const *)dce;
        if (!drv) continue;
        drv = MT_UNITS_PTR(drv, mask);
        if (dce[sizeof(void *) + 1] & 0x40) {      /* dRAMBased: dCtlDriver is a handle */
            drv = *(const uint8_t *const *)drv;
            if (!drv) continue;                    /* purged */
            drv = MT_UNITS_PTR(drv, mask);
        }
        if (drv[18] == 4 && drv[19] == '.' && (drv[20] | 0x20) == 'i' && (drv[21] | 0x20) == 'p' &&
            (drv[22] | 0x20) == 'p') {
            if (dce_out) *dce_out = dce;
            return drv;
        }
    }
    return NULL;
}

static inline const uint8_t *mt_units_find_ipp_in(uint8_t **const *tbl, int n, uintptr_t mask) {
    return mt_units_find_ipp_dce_in(tbl, n, mask, NULL);
}

/* MoofTCP's status view (mt_view.h) from its DCE's dCtlStorage, or NULL (none: a driver that
 * predates it, or another layout). Call only for a unit mt_units_is_moof accepted. */
static inline const MtStatusView *mt_units_view_in(const uint8_t *dce, uintptr_t mask) {
    const MtStatusView *v;
    if (!dce) return NULL;
    v = *(const MtStatusView *const *)(dce + MT_UNITS_DCE_STORAGE);
    if (!v) return NULL;
    v = (const MtStatusView *)MT_UNITS_PTR(v, mask);
    if (v->magic != MT_VIEW_MAGIC || v->version != MT_VIEW_VERSION) return NULL;
    return v;
}

#if defined(__m68k__)
/* The driver header of the first unit whose driver is named ".ipp" (any case), or NULL. */
static inline const uint8_t *mt_units_find_ipp(void) {
    return mt_units_find_ipp_in(*(uint8_t **const *volatile *)0x11C, *(volatile int16_t *)0x1D2,
                                MT_UNITS_MASK24);
}
static inline const uint8_t *mt_units_find_ipp_dce(const uint8_t **dce_out) {
    return mt_units_find_ipp_dce_in(*(uint8_t **const *volatile *)0x11C, *(volatile int16_t *)0x1D2,
                                    MT_UNITS_MASK24, dce_out);
}
#endif

/* 1 if drv (from mt_units_find_ipp) is MoofTCP's driver: its open entry is at offset 0x18 and
 * starts with MoofTCP's stub "moveq #0,d0; bra.s 48" (mac/moof/moof.r; tests/test_cdev_logic.c
 * reads the header from moof.r so a change there breaks the test). MacTCP's open entry is 0x32. */
static inline int mt_units_is_moof(const uint8_t *drv) {
    return drv && drv[8] == 0x00 && drv[9] == 0x18 && drv[24] == 0x70 && drv[25] == 0x00 &&
           drv[26] == 0x60 && drv[27] == 0x14;
}

#endif
