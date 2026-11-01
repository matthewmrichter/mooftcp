/* cfg_edit.c - see cfg_edit.h. */
#include <string.h>
#include "cfg_edit.h"
#include "mt_cfg.h"

static int terminated(const char *s, size_t cap) {
    size_t i;
    for (i = 0; i < cap; i++) if (!s[i]) return 1;
    return 0;
}

int cfe_parse_ip(const char *s, mt_ip4 *out) {
    uint32_t v = 0, f, digits;
    int i;
    if (!s) return 0;
    for (i = 0; i < 4; i++) {
        f = 0; digits = 0;
        if (*s == '0' && s[1] >= '0' && s[1] <= '9') return 0;   /* leading zero */
        while (*s >= '0' && *s <= '9') {
            if (digits == 3) return 0;
            f = f * 10u + (uint32_t)(*s - '0');
            digits++; s++;
        }
        if (digits == 0 || f > 255u) return 0;
        v = (v << 8) | f;
        if (i < 3) { if (*s != '.') return 0; s++; }
    }
    if (*s) return 0;
    *out = v;
    return 1;
}

int cfe_parse_mask(const char *s, mt_ip4 *out) {
    mt_ip4 m;
    if (!cfe_parse_ip(s, &m) || !mt_cfg_mask_valid(m)) return 0;
    *out = m;
    return 1;
}

static void put_dec(char **p, uint32_t v) {
    if (v >= 100u) *(*p)++ = (char)('0' + v / 100u);
    if (v >= 10u) *(*p)++ = (char)('0' + (v / 10u) % 10u);
    *(*p)++ = (char)('0' + v % 10u);
}

void cfe_format_ip(mt_ip4 a, char out[16]) {
    char *p = out;
    int i;
    for (i = 3; i >= 0; i--) {
        put_dec(&p, (a >> (8 * i)) & 255u);
        if (i) *p++ = '.';
    }
    *p = 0;
}

/* class check for gateway and DNS (no subnet rules) */
static int plain_ok(mt_ip4 a) {
    return a >= 0x01000000u && a < 0xE0000000u && (a >> 24) != 127u;
}

static int domain_ok(const char *d) {
    size_t i, n = 0;
    while (n < 64 && d[n]) n++;
    if (n > 63) return 0;
    if (n == 0) return 1;
    if (d[0] == '.' || d[n - 1] == '.') return 0;
    for (i = 0; i < n; i++) {
        char c = d[i];
        int ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 c == '-' || c == '.' || c == '_';
        if (!ok) return 0;
        if (c == '.' && d[i + 1] == '.') return 0;
    }
    return 1;
}

static int fail(CfeError *e, int field, int code) {
    if (e) { e->field = field; e->code = code; }
    return code;
}

int cfe_validate(const CfeFields *f, CfeError *err) {
    mt_ip4 ip = 0, mask = 0, gw = 0, dns = 0;
    int have_ip, have_mask, have_gw, have_dns, offnet = 0;
    if (err) { err->field = CFE_F_NONE; err->code = CFE_OK; }
    if (!terminated(f->ip, sizeof f->ip)) return fail(err, CFE_F_IP, CFE_E_IP);
    if (!terminated(f->mask, sizeof f->mask)) return fail(err, CFE_F_MASK, CFE_E_MASK);
    if (!terminated(f->gw, sizeof f->gw)) return fail(err, CFE_F_GW, CFE_E_GW);
    if (!terminated(f->dns, sizeof f->dns)) return fail(err, CFE_F_DNS, CFE_E_DNS);
    if (!terminated(f->domain, sizeof f->domain)) return fail(err, CFE_F_DOMAIN, CFE_E_DOMAIN);
    have_ip = f->ip[0] != 0; have_mask = f->mask[0] != 0;
    have_gw = f->gw[0] != 0; have_dns = f->dns[0] != 0;

    if (have_ip && !cfe_parse_ip(f->ip, &ip)) return fail(err, CFE_F_IP, CFE_E_IP);
    if (have_mask && !cfe_parse_mask(f->mask, &mask)) return fail(err, CFE_F_MASK, CFE_E_MASK);
    if (have_gw && !cfe_parse_ip(f->gw, &gw)) return fail(err, CFE_F_GW, CFE_E_GW);
    if (have_dns && !cfe_parse_ip(f->dns, &dns)) return fail(err, CFE_F_DNS, CFE_E_DNS);
    if (!domain_ok(f->domain)) return fail(err, CFE_F_DOMAIN, CFE_E_DOMAIN);

    if (!have_ip) {
        if (have_mask || have_gw || have_dns) return fail(err, CFE_F_IP, CFE_E_IP);
        return CFE_OK;
    }
    if (!mask) mask = mt_cfg_class_mask(ip);
    if (!plain_ok(ip)) return fail(err, CFE_F_IP, CFE_E_ADDR_RESERVED);
    if (!mt_cfg_addr_valid(ip, mask)) return fail(err, CFE_F_IP, CFE_E_HOSTBITS);
    if (have_gw) {
        if (!plain_ok(gw)) return fail(err, CFE_F_GW, CFE_E_ADDR_RESERVED);
        if (gw == ip) return fail(err, CFE_F_GW, CFE_E_GW_SELF);
        if ((gw & mask) == (ip & mask)) {
            if (!mt_cfg_addr_valid(gw, mask)) return fail(err, CFE_F_GW, CFE_E_ADDR_RESERVED);
        } else offnet = 1;
    }
    if (have_dns && !plain_ok(dns)) return fail(err, CFE_F_DNS, CFE_E_ADDR_RESERVED);
    if (offnet) return fail(err, CFE_F_GW, CFE_E_GW_OFFNET);
    return CFE_OK;
}

uint32_t cfe_encode(const CfeFields *f, const uint8_t *prior, uint32_t prior_len, uint8_t *out,
                    uint32_t cap) {
    mt_ifconfig c, junk;
    uint16_t mode = MT_CFG_MODE_MANUAL;
    uint8_t flags = 0, pm;
    const uint8_t *tail = NULL;
    uint32_t tail_len = 0;
    if (cfe_is_fatal(cfe_validate(f, NULL))) return 0;
    memset(&c, 0, sizeof c);
    if (f->ip[0]) cfe_parse_ip(f->ip, &c.addr);
    if (f->mask[0]) cfe_parse_mask(f->mask, &c.mask);
    if (f->gw[0]) cfe_parse_ip(f->gw, &c.gateway);
    if (f->dns[0]) cfe_parse_ip(f->dns, &c.dns);
    memcpy(c.domain, f->domain, sizeof c.domain);
    c.mtu = 1500;
    if (prior && mt_cfg_parse_flags(prior, prior_len, &junk, &pm, &flags) == 0) {
        mode = mt_cfg_raw_mode(prior, prior_len);
        tail = mt_cfg_tail(prior, prior_len, &tail_len);
    } else {
        flags = 0;
    }
    flags = (uint8_t)((flags & (uint8_t)~MT_CFG_FLAG_LOG) | (f->log ? MT_CFG_FLAG_LOG : 0));
    return mt_cfg_build_keep(&c, mode, flags, tail, tail_len, out, cap);
}

int cfe_decode(const uint8_t *res, uint32_t n, CfeFields *f) {
    mt_ifconfig c;
    uint8_t mode, flags;
    memset(f, 0, sizeof *f);
    if (mt_cfg_parse_flags(res, n, &c, &mode, &flags) != 0) return -1;
    if (c.addr) {
        cfe_format_ip(c.addr, f->ip);
        cfe_format_ip(c.mask, f->mask);
    }
    if (plain_ok(c.gateway)) cfe_format_ip(c.gateway, f->gw);
    if (plain_ok(c.dns)) cfe_format_ip(c.dns, f->dns);
    if (domain_ok(c.domain)) memcpy(f->domain, c.domain, sizeof f->domain);
    f->log = (uint8_t)(flags & MT_CFG_FLAG_LOG ? 1 : 0);
    return 0;
}
