/* cfg_edit.h - control panel field logic (Plan 3 Task 7; portable: no Toolbox, no malloc).
 *
 * The control panel edits five text fields (IP, mask, gateway, DNS, domain). This module parses
 * and validates them strictly and converts them to and from the MCFG 128 resource of
 * "MoofTCP Prefs" (mac/glue/mt_cfg.h), reusing that header's writer and checks so the panel and
 * the driver's loader can never disagree about what a usable configuration is.
 *
 * Rulings (Plan 3 Task 7, Review Focus 3):
 *  - Dotted quad: exactly 4 decimal fields, 1-3 digits each, value 0-255, no spaces or other
 *    characters, no empty field, no leading zeros ("01" is rejected: it is ambiguous octal).
 *  - Mask: a dotted quad that is one run of 1 bits (not 0). 255.255.255.255 is allowed.
 *  - All five fields empty is valid ("the stack stays down"). A domain alone is valid.
 *  - IP set, mask empty: valid, the loader uses the class mask; every IP check uses that mask.
 *  - IP empty but mask, gateway or DNS set: error CFE_E_IP (field IP). Ignoring typed values
 *    silently would hide a mistake, and a stack without an address cannot use them.
 *  - IP must be a unicast host address (mt_cfg_addr_valid): not 0.x, 127/8, >= 224.0.0.0, and not
 *    the network or broadcast address of its subnet (mask shorter than /31). 0.0.0.0 typed is
 *    CFE_E_ADDR_RESERVED (empty means unconfigured), a bad host part is CFE_E_HOSTBITS.
 *  - Gateway and DNS, when non-empty: 0.x, 127/8 and >= 224.0.0.0 are CFE_E_ADDR_RESERVED. The
 *    gateway must not be our own address (CFE_E_GW_SELF) nor the network/broadcast address of
 *    our subnet (CFE_E_ADDR_RESERVED). A gateway outside our subnet is only a warning
 *    (CFE_E_GW_OFFNET, a non-fatal result): the caller shows it and decides.
 *  - Domain: up to 63 characters of letters, digits, '-', '.', '_'; no leading or trailing dot, no
 *    "..", (the resolver appends it to names). Empty is fine.
 *  - Strings must be NUL-terminated inside their buffer, else the field is invalid.
 *  - cfe_encode refuses (returns 0) on any fatal validation result. It keeps the raw mode word,
 *    the flags byte's other bits and unknown tail bytes of the prior resource (R3); bit 0 (the
 *    diagnostic log) comes from f->log (Task 11b). A prior that is not a usable v1 resource
 *    (corrupt, foreign version, too short) counts as none: mode manual, flags = f->log only, no
 *    tail.
 *  - cfe_decode sets f->log from flags bit 0 (0 when the resource has no flags byte). The panel's
 *    default for a NEW file (no usable resource) is log on: cdl_load_saved (cdev_logic.h).
 *  - cfe_decode shows what is stored but blanks fields the validator would refuse (an address the
 *    loader treats as unconfigured, a reserved gateway/DNS, a bad domain). */
#ifndef CFG_EDIT_H
#define CFG_EDIT_H

#include <stdint.h>
#include "mt.h"

/* log: the "Keep a diagnostic log" checkbox = prefs flags bit 0 (MT_CFG_FLAG_LOG), 0 or 1 (Task 11b). */
typedef struct { char ip[16], mask[16], gw[16], dns[16], domain[64]; uint8_t log; } CfeFields;

/* field ids (CfeError.field) */
enum { CFE_F_NONE = 0, CFE_F_IP, CFE_F_MASK, CFE_F_GW, CFE_F_DNS, CFE_F_DOMAIN };

/* result codes: 0 = ok, 1..99 fatal (cannot save), 100.. warnings (may save) */
enum {
    CFE_OK = 0,
    CFE_E_IP = 1,            /* not a dotted quad, or missing while another field is set */
    CFE_E_MASK,              /* not a dotted quad, 0, or not contiguous */
    CFE_E_GW,                /* gateway is not a dotted quad */
    CFE_E_DNS,               /* DNS is not a dotted quad */
    CFE_E_DOMAIN,            /* too long, bad character or bad dots */
    CFE_E_ADDR_RESERVED,     /* 0.x, 127/8, >= 224.0.0.0, or a subnet's network/broadcast gateway */
    CFE_E_HOSTBITS,          /* our address has host bits all 0 or all 1 */
    CFE_E_GW_SELF,           /* gateway equals our address */
    CFE_E_GW_OFFNET = 100    /* warning: gateway not on our subnet */
};

typedef struct { int field; int code; } CfeError;

/* 1 = ok, 0 = bad; *out only written on success. */
int cfe_parse_ip(const char *s, mt_ip4 *out);
int cfe_parse_mask(const char *s, mt_ip4 *out);
void cfe_format_ip(mt_ip4 a, char out[16]);        /* "a.b.c.d" */

/* 0 = fine, otherwise a CFE_E_* code (also in err->field/code when err is not NULL; err is zeroed
 * on 0). CFE_E_GW_OFFNET is non-fatal: use cfe_is_fatal. If both a fatal error and the warning
 * apply, the fatal one is reported. */
int cfe_validate(const CfeFields *f, CfeError *err);
static inline int cfe_is_fatal(int rc) { return rc != CFE_OK && rc != CFE_E_GW_OFFNET; }

/* Encode f as an MCFG resource into out (cap bytes). Returns the length, or 0 on a fatal
 * validation result or too small a cap. prior may be NULL (new file). */
uint32_t cfe_encode(const CfeFields *f, const uint8_t *prior, uint32_t prior_len, uint8_t *out,
                    uint32_t cap);
/* Fill f from a resource; 0 on success, -1 if it is not a usable resource (f is then all empty). */
int cfe_decode(const uint8_t *res, uint32_t n, CfeFields *f);

#endif
