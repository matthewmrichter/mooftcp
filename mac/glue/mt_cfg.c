/* mt_cfg.c - read "MoofTCP Prefs" (resource MCFG 128) from the System Folder at boot. The format
 * and the parser are in mt_cfg.h. Called from the INIT path (mt_drv_boot), so the Resource
 * Manager is usable; the current resource file is restored before returning. */
#include <Resources.h>
#include <Memory.h>
#include <Files.h>
#include "mt_cfg.h"

#pragma GCC diagnostic ignored "-Warray-bounds"
#define LM_BOOTDRIVE (*(volatile int16_t *)0x210)   /* working directory of the System Folder */

int mt_cfg_load(mt_ifconfig *c, uint8_t *mode) {
    short saved = CurResFile(), ref;
    Handle h;
    int r = MT_CFG_NORES;
    memset(c, 0, sizeof *c);
    *mode = 0;
    ref = OpenRFPerm((ConstStringPtr)"\pMoofTCP Prefs", LM_BOOTDRIVE, fsRdPerm);
    if (ref == -1) {
        UseResFile(saved);
        return MT_CFG_NOFILE;
    }
    h = Get1Resource('MCFG', 128);
    if (h) {
        HLock(h);
        if (mt_cfg_parse((const uint8_t *)*h, (uint32_t)GetHandleSize(h), c, mode) == 0) r = MT_CFG_OK;
        HUnlock(h);
        ReleaseResource(h);
    }
    CloseResFile(ref);
    UseResFile(saved);
    return r;
}
