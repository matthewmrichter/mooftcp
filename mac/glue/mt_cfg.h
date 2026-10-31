/* mt_cfg.h - the "MoofTCP Prefs" settings (Plan 2 ruling P3, Task 10).
 *
 * The file ":System Folder:MoofTCP Prefs" (written by scripts/mkprefs.sh until Plan 3's control
 * panel writes the same format) holds resource 'MCFG' 128. Every field is optional: a shorter
 * resource leaves the missing trailing fields 0, and 0 means "not set". Big-endian:
 *
 *   offset  size  field
 *        0     2  version (MT_CFG_VERSION; any other value: the resource is ignored)
 *        2     2  mode: 0 manual, 1 DHCP (Plan 4; read and kept, no DHCP yet)
 *        4     4  IP address (0 = none: the driver stays unconfigured)
 *        8     4  subnet mask (0 = the address's class mask)
 *       12     4  gateway (0 = none)
 *       16     4  DNS server (0 = none)
 *       20   1+n  domain, Pascal string (n <= 63; a longer one is ignored)
 *
 * mt_cfg_parse is pure (host-tested in tests/test_cfg.c); mt_cfg_load (mt_cfg.c, Toolbox) reads
 * the file at boot. */
#ifndef MT_CFG_H
#define MT_CFG_H

#include <stdint.h>
#include <string.h>
#include "mt.h"

#define MT_CFG_VERSION     1u
#define MT_CFG_MODE_MANUAL 0u
#define MT_CFG_MODE_DHCP   1u
#define MT_CFG_MIN_LEN     2u        /* the version word */

/* mt_cfg_load results */
#define MT_CFG_OK       0
#define MT_CFG_NOFILE   1            /* no "MoofTCP Prefs" in the System Folder */
#define MT_CFG_NORES    2            /* the file has no usable MCFG 128 */

static inline uint32_t mt_cfg_rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* The classful mask of an address (A /8, B /16, C /24; D and E: /32). */
static inline mt_ip4 mt_cfg_class_mask(mt_ip4 a) {
    if (a < 0x80000000u) return 0xFF000000u;
    if (a < 0xC0000000u) return 0xFFFF0000u;
    if (a < 0xE0000000u) return 0xFFFFFF00u;
    return 0xFFFFFFFFu;
}

/* Fill *c (MAC left zero, mtu 1500) and *mode from an MCFG resource of n bytes. 0 on success,
 * -1 if the resource is unusable (shorter than the version word, or another version); *c and
 * *mode are then all zero (unconfigured). */
static inline int mt_cfg_parse(const uint8_t *p, uint32_t n, mt_ifconfig *c, uint8_t *mode) {
    uint8_t b[20];
    uint32_t k;
    memset(c, 0, sizeof *c);
    *mode = 0;
    if (!p || n < MT_CFG_MIN_LEN || ((uint32_t)p[0] << 8 | p[1]) != MT_CFG_VERSION) return -1;
    memset(b, 0, sizeof b);
    memcpy(b, p, n < sizeof b ? n : sizeof b);
    *mode = (uint8_t)(((uint32_t)b[2] << 8 | b[3]) == MT_CFG_MODE_DHCP ? MT_CFG_MODE_DHCP
                                                                         : MT_CFG_MODE_MANUAL);
    c->addr = mt_cfg_rd32(b + 4);
    c->mask = mt_cfg_rd32(b + 8);
    c->gateway = mt_cfg_rd32(b + 12);
    c->dns = mt_cfg_rd32(b + 16);
    if (c->addr && !c->mask) c->mask = mt_cfg_class_mask(c->addr);
    c->mtu = 1500;
    if (n > 20) {
        k = p[20];
        if (k < sizeof c->domain && 21u + k <= n) {
            memcpy(c->domain, p + 21, k);
            c->domain[k] = 0;
        }
    }
    return 0;
}

int mt_cfg_load(mt_ifconfig *c, uint8_t *mode);   /* Mac only: MT_CFG_OK / _NOFILE / _NORES */

#endif /* MT_CFG_H */
