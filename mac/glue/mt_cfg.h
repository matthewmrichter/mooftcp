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
 *     21+n     1  flags (optional): bit 0 = diagnostic log on (mt_log_enable at boot); other bits
 *                  reserved, kept by writers
 *     22+n   ...  unknown trailing bytes: ignored by the loader, kept by writers (mt_cfg_tail)
 *
 * Loader rulings (Task 6): an address that is 0.x.x.x, 127/8 or >= 224.0.0.0, host bits all 0 or all 1 (mask shorter than /31),
 * or a mask that is not one run of 1 bits, makes the resource unconfigured (address 0), everything else is kept. *
 * mt_cfg_parse is pure (host-tested); mt_cfg_load (mt_cfg.c, Toolbox) reads
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
#define MT_CFG_FLAG_LOG    0x01u     /* flags byte bit 0: diagnostic log on (Plan 3 Task 6) */

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

/* Address and mask checks the loader and the control panel share (Plan 3 Tasks 6 and 7). An
 * address is usable as our own only if it is a unicast host address: not 0.x.x.x ("this
 * network"), not 127/8 (the core loops 127/8 back and drops it as a wire source, so it can never
 * be the interface address), not >= 224.0.0.0 (class D/E, 255.255.255.255 included: the core's
 * UDP queue marker relies on that value never being our address) and, when the mask is shorter
 * than /31, not the network address or the directed broadcast of its own subnet (host bits all 0
 * or all 1: such a configuration is a silently dead network). mask is the effective mask (0 =
 * the class default). It is not checked for being contiguous here (mt_cfg_mask_valid). A mask is
 * usable if it is one run of 1 bits from the top. */
static inline int mt_cfg_addr_valid(mt_ip4 a, mt_ip4 mask) {
    mt_ip4 host;
    if (!(a >= 0x01000000u && a < 0xE0000000u && (a >> 24) != 127u)) return 0;
    if (!mask) mask = mt_cfg_class_mask(a);
    host = a & ~mask;
    if (~mask > 1u && (host == 0 || host == ~mask)) return 0;
    return 1;
}
static inline int mt_cfg_mask_valid(mt_ip4 m) {
    mt_ip4 inv = ~m;                         /* contiguous iff ~m is 2^k - 1 */
    return m != 0 && (inv & (inv + 1u)) == 0;
}

/* Where the optional parts of a parsed resource of n bytes start (0 = absent). The domain string
 * must fit (length byte <= 63 and inside n) for anything behind it to be addressed. */
static inline uint32_t mt_cfg_flags_off(const uint8_t *p, uint32_t n) {
    uint32_t k;
    if (!p || n <= 20u) return 0;
    k = p[20];
    if (k >= 64u || 21u + k > n) return 0;
    return 21u + k;                          /* the flags byte (valid if < n) */
}

/* Fill *c (MAC left zero, mtu 1500), *mode (0/1) and *flags (0 when absent) from an MCFG resource
 * of n bytes. 0 on success, -1 if the resource is unusable (shorter than the version word, or
 * another version); *c, *mode and *flags are then all zero (unconfigured). An unusable address or
 * mask (mt_cfg_addr_valid / mt_cfg_mask_valid) leaves addr and mask 0 = unconfigured, everything
 * else is kept. A 32-bit field cut off mid-word counts as missing (0). flags may be NULL. Unknown bytes behind the flags byte are ignored. */
static inline int mt_cfg_parse_flags(const uint8_t *p, uint32_t n, mt_ifconfig *c, uint8_t *mode,
                                     uint8_t *flags) {
    uint8_t b[20];
    uint32_t k, fo;
    memset(c, 0, sizeof *c);
    *mode = 0;
    if (flags) *flags = 0;
    if (!p || n < MT_CFG_MIN_LEN || ((uint32_t)p[0] << 8 | p[1]) != MT_CFG_VERSION) return -1;
    memset(b, 0, sizeof b);
    memcpy(b, p, n < sizeof b ? n & ~1u : sizeof b);   /* whole 16-bit words only, see below */
    for (k = 4; k < 20; k += 4)                         /* a field cut off mid-word is not set */
        if (n < k + 4u) memset(b + k, 0, 20u - k);
    *mode = (uint8_t)(((uint32_t)b[2] << 8 | b[3]) == MT_CFG_MODE_DHCP ? MT_CFG_MODE_DHCP
                                                                         : MT_CFG_MODE_MANUAL);
    c->addr = mt_cfg_rd32(b + 4);
    c->mask = mt_cfg_rd32(b + 8);
    c->gateway = mt_cfg_rd32(b + 12);
    c->dns = mt_cfg_rd32(b + 16);
    if (c->addr && !c->mask) c->mask = mt_cfg_class_mask(c->addr);
    if (c->addr && (!mt_cfg_addr_valid(c->addr, c->mask) || !mt_cfg_mask_valid(c->mask))) c->addr = 0;
    if (!c->addr) c->mask = 0;
    c->mtu = 1500;
    if (n > 20) {
        k = p[20];
        if (k < sizeof c->domain && 21u + k <= n) {
            memcpy(c->domain, p + 21, k);
            c->domain[k] = 0;
        }
    }
    fo = mt_cfg_flags_off(p, n);
    if (flags && fo && fo < n) *flags = p[fo];
    return 0;
}

static inline int mt_cfg_parse(const uint8_t *p, uint32_t n, mt_ifconfig *c, uint8_t *mode) {
    return mt_cfg_parse_flags(p, n, c, mode, NULL);
}

/* Preservation helpers for a writer that rewrites an existing resource (the control panel, R3):
 * the raw mode word (the parser folds anything but 1 to manual), and the bytes behind the flags
 * byte. *len is 0 and NULL is returned when there are none (or the layout cannot be followed). */
static inline uint16_t mt_cfg_raw_mode(const uint8_t *p, uint32_t n) {
    if (!p || n < 4u || ((uint32_t)p[0] << 8 | p[1]) != MT_CFG_VERSION) return 0;
    return (uint16_t)((uint32_t)p[2] << 8 | p[3]);
}
static inline const uint8_t *mt_cfg_tail(const uint8_t *p, uint32_t n, uint32_t *len) {
    uint32_t fo = mt_cfg_flags_off(p, n);
    *len = 0;
    if (!fo || fo + 1u >= n) return NULL;
    *len = n - (fo + 1u);
    return p + fo + 1u;
}

/* Write an MCFG v1 body: version, mode word, addr, mask, gateway, dns, domain (Pascal), flags
 * byte, then tail_len bytes of tail (an earlier resource's unknown trailing bytes; tail may be
 * NULL). Returns the length, or 0 if cap is too small or the domain is not a string of <= 63
 * characters. Fields are written as given (no validation: the loader validates). */
static inline uint32_t mt_cfg_build_keep(const mt_ifconfig *c, uint16_t mode_word, uint8_t flags,
                                         const uint8_t *tail, uint32_t tail_len, uint8_t *out,
                                         uint32_t cap) {
    uint32_t dl = 0, need;
    while (dl < sizeof c->domain && c->domain[dl]) dl++;
    if (dl > 63u) return 0;
    if (!tail) tail_len = 0;
    need = 22u + dl + tail_len;
    if (!out || cap < need) return 0;
    out[0] = 0; out[1] = (uint8_t)MT_CFG_VERSION;
    out[2] = (uint8_t)(mode_word >> 8); out[3] = (uint8_t)mode_word;
    { mt_ip4 v[4]; int i; v[0] = c->addr; v[1] = c->mask; v[2] = c->gateway; v[3] = c->dns;
      for (i = 0; i < 4; i++) {
          out[4 + 4 * i] = (uint8_t)(v[i] >> 24); out[5 + 4 * i] = (uint8_t)(v[i] >> 16);
          out[6 + 4 * i] = (uint8_t)(v[i] >> 8);  out[7 + 4 * i] = (uint8_t)v[i];
      } }
    out[20] = (uint8_t)dl;
    memcpy(out + 21, c->domain, dl);
    out[21 + dl] = flags;
    if (tail_len) memcpy(out + 22 + dl, tail, tail_len);
    return need;
}

/* The plain writer: mode 0/1, flags = bit 0 (MT_CFG_FLAG_LOG) from log_on. */
static inline uint32_t mt_cfg_build(const mt_ifconfig *c, uint8_t mode, uint8_t log_on, uint8_t *out,
                                    uint32_t cap) {
    return mt_cfg_build_keep(c, mode, (uint8_t)(log_on ? MT_CFG_FLAG_LOG : 0), NULL, 0, out, cap);
}

int mt_cfg_load(mt_ifconfig *c, uint8_t *mode, uint8_t *flags);   /* Mac only: MT_CFG_OK / _NOFILE / _NORES */

#endif /* MT_CFG_H */
