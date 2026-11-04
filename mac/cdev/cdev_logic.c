/* cdev_logic.c - see cdev_logic.h. No libc calls beyond what the compiler may emit (the cdev is
 * built position independent; check_pcrel.sh covers memcpy/memset when they are reached). */
#include "cdev_logic.h"

int cdl_field_cap(int field) {
    switch (field) {
    case CFE_F_IP: case CFE_F_MASK: case CFE_F_GW: case CFE_F_DNS: return 15;
    case CFE_F_DOMAIN: return 63;
    default: return 0;
    }
}

int cdl_insert_ok(int cur, int sel, int n, int cap) {
    if (sel > cur) sel = cur;
    if (sel < 0) sel = 0;
    return cur - sel + n <= cap;
}

int cdl_key_inserts(unsigned char c) {
    return c >= 32 && c != 127;
}

void cdl_pcat(unsigned char *ps, const char *c) {
    while (*c && ps[0] < 255) ps[++ps[0]] = (unsigned char)*c++;
}

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int cdl_fields_equal(const CfeFields *a, const CfeFields *b) {
    return streq(a->ip, b->ip) && streq(a->mask, b->mask) && streq(a->gw, b->gw) &&
           streq(a->dns, b->dns) && streq(a->domain, b->domain) && !a->log == !b->log &&
           !a->dhcp == !b->dhcp;
}

void cdl_load_saved(const uint8_t *res, uint32_t n, CfeFields *f) {
    if (cfe_decode(res, n, f) != 0) f->log = 1;         /* no usable settings yet: log on */
}

static const char *field_name(int field) {
    switch (field) {
    case CFE_F_IP: return "IP address";
    case CFE_F_MASK: return "subnet mask";
    case CFE_F_GW: return "router address";
    case CFE_F_DNS: return "DNS server address";
    default: return "local domain";
    }
}

void cdl_problem_text(const CfeFields *f, int rc, const CfeError *e, unsigned char *ps) {
    ps[0] = 0;
    switch (rc) {
    case CFE_OK: break;
    case CFE_E_IP:
        if (!f->ip[0]) cdl_pcat(ps, "IP address required: the other fields are set.");
        else cdl_pcat(ps, "The IP address is invalid. Type four numbers 0-255, like 192.168.1.20.");
        break;
    case CFE_E_MASK: cdl_pcat(ps, "The subnet mask is invalid (for example 255.255.255.0)."); break;
    case CFE_E_GW: cdl_pcat(ps, "The router address is invalid."); break;
    case CFE_E_DNS: cdl_pcat(ps, "The DNS server address is invalid."); break;
    case CFE_E_DOMAIN:
        cdl_pcat(ps, "The local domain is invalid: letters, digits, - _ and inner dots, up to 63.");
        break;
    case CFE_E_ADDR_RESERVED:
        cdl_pcat(ps, "The ");
        cdl_pcat(ps, field_name(e ? e->field : 0));
        cdl_pcat(ps, " is a reserved address.");
        break;
    case CFE_E_HOSTBITS:
        cdl_pcat(ps, "The IP address is the network or broadcast address of its subnet.");
        break;
    case CFE_E_GW_SELF: cdl_pcat(ps, "The router address is the IP address itself."); break;
    case CFE_E_GW_OFFNET: cdl_pcat(ps, "Note: the router is not on the IP address's subnet."); break;
    default: cdl_pcat(ps, "The settings are invalid."); break;
    }
}

int cdl_check_key(const CfeFields *f, int rc, const CfeError *e) {
    int field, ipempty;
    if (rc == CFE_OK) return 0;
    field = e ? e->field : 0;
    ipempty = (rc == CFE_E_IP && !f->ip[0]) ? 1 : 0;
    return (rc << 4) | (field << 1) | ipempty;    /* rc < 128, field < 8 */
}

int cdl_save_action(int rc) {
    if (cfe_is_fatal(rc)) return CDL_SAVE_REFUSE;
    return rc == CFE_E_GW_OFFNET ? CDL_SAVE_ASK_WARN : CDL_SAVE_WRITE;
}

int cdl_close_action(const CfeFields *cur, const CfeFields *saved, const CfeFields *draft, int rc) {
    if (cdl_fields_equal(cur, saved)) return draft ? CDL_CLOSE_DROP_DRAFT : CDL_CLOSE_NOTHING;
    if (!cfe_is_fatal(rc)) return CDL_CLOSE_ASK;
    if (draft && cdl_fields_equal(cur, draft)) return CDL_CLOSE_NOTHING;   /* already kept */
    return CDL_CLOSE_KEEP_DRAFT;
}

static const char *fld(const CfeFields *f, int i) {
    switch (i) {
    case 0: return f->ip;
    case 1: return f->mask;
    case 2: return f->gw;
    case 3: return f->dns;
    default: return f->domain;
    }
}

static char *fldw(CfeFields *f, int i, int *cap) {
    switch (i) {
    case 0: *cap = (int)sizeof f->ip; return f->ip;
    case 1: *cap = (int)sizeof f->mask; return f->mask;
    case 2: *cap = (int)sizeof f->gw; return f->gw;
    case 3: *cap = (int)sizeof f->dns; return f->dns;
    default: *cap = (int)sizeof f->domain; return f->domain;
    }
}

uint32_t cdl_draft_encode(const CfeFields *f, uint8_t *out, uint32_t cap) {
    uint32_t n = 1;
    int i;
    if (cap < 1) return 0;
    out[0] = 3;
    for (i = 0; i < 5; i++) {
        const char *s = fld(f, i);
        uint32_t k = 0;
        while (k < 63 && s[k]) k++;
        if (n + 1 + k > cap) return 0;
        out[n++] = (uint8_t)k;
        while (k--) out[n++] = (uint8_t)*s++;
    }
    if (n + 2 > cap) return 0;
    out[n++] = f->log ? 1 : 0;                          /* version 2: the log flag */
    out[n++] = f->dhcp ? 1 : 0;                         /* version 3: the DHCP flag */
    return n;
}

int cdl_draft_decode(const uint8_t *p, uint32_t n, CfeFields *f, int default_log) {
    uint32_t off = 1;
    int i, cap;
    char *d;
    char *z = (char *)f;
    uint32_t k;
    for (k = 0; k < sizeof *f; k++) z[k] = 0;
    if (!p || n < 1 || p[0] < 1 || p[0] > 3) return -1;
    for (i = 0; i < 5; i++) {
        if (off >= n) goto bad;
        k = p[off++];
        d = fldw(f, i, &cap);
        if ((int)k >= cap || off + k > n) goto bad;
        {
            uint32_t j;
            for (j = 0; j < k; j++) {
                if (!p[off + j]) goto bad;
                d[j] = (char)p[off + j];
            }
            d[k] = 0;
        }
        off += k;
    }
    if (p[0] >= 2) {                                    /* version 2: one flag byte, 0 or 1 */
        if (off >= n || p[off] > 1) goto bad;
        f->log = p[off++];
        if (p[0] == 3) {                                /* version 3: the DHCP byte, 0 or 1 */
            if (off >= n || p[off] > 1) goto bad;
            f->dhcp = p[off];
        }
    } else {
        f->log = (uint8_t)(default_log ? 1 : 0);        /* version 1 drafts had no flag */
    }
    return 0;
bad:
    for (k = 0; k < sizeof *f; k++) z[k] = 0;
    return -1;
}

/* "a.b.c.d" into ps (no libc: the cdev is position independent). */
static void pcat_num(unsigned char *ps, uint32_t v) {
    char t[11];
    int i = 10;
    t[10] = 0;
    do { t[--i] = (char)('0' + v % 10u); v /= 10u; } while (v && i > 0);
    cdl_pcat(ps, t + i);
}

static void pcat_ip(unsigned char *ps, uint32_t a) {
    int k;
    for (k = 3; k >= 0; k--) {
        pcat_num(ps, (a >> (8 * k)) & 0xFFu);
        if (k) cdl_pcat(ps, ".");
    }
}

void cdl_status_text(int unit, const MtStatusView *v, uint32_t now_ticks, unsigned char *ps) {
    ps[0] = 0;
    if (unit == CDL_UNIT_NONE) { cdl_pcat(ps, "Not loaded: restart after saving."); return; }
    if (unit == CDL_UNIT_MACTCP) { cdl_pcat(ps, "MacTCP is loaded; MoofTCP is off."); return; }
    if (!v) { cdl_pcat(ps, "MoofTCP is loaded."); return; }
    if (!v->mode) {
        if (!v->addr) { cdl_pcat(ps, "MoofTCP is loaded, not configured."); return; }
        cdl_pcat(ps, v->linked ? "MoofTCP is running at " : "MoofTCP is loaded: ");
        pcat_ip(ps, v->addr);
        cdl_pcat(ps, ".");
        return;
    }
    if (!v->linked || v->state == MT_DHCP_OFF) {
        cdl_pcat(ps, "DHCP: starts with the first network app.");
        return;
    }
    if (v->state < MT_DHCP_BOUND || !v->addr) { cdl_pcat(ps, "DHCP: asking for an address..."); return; }
    cdl_pcat(ps, "DHCP: ");
    pcat_ip(ps, v->addr);
    if (v->state != MT_DHCP_BOUND) { cdl_pcat(ps, ", renewing"); return; }
    if (v->lease_s == 0xFFFFFFFFu) { cdl_pcat(ps, " (no expiry)"); return; }
    {
        uint32_t el = (now_ticks - v->t_bound) / 60u, left = v->lease_s > el ? v->lease_s - el : 0u;
        cdl_pcat(ps, " (lease ");
        if (left >= 2u * 86400u) {
            pcat_num(ps, left / 86400u);
            cdl_pcat(ps, "d");
        } else {
            if (left >= 3600u) { pcat_num(ps, left / 3600u); cdl_pcat(ps, "h"); }
            pcat_num(ps, left % 3600u / 60u);
            cdl_pcat(ps, "m");
        }
        cdl_pcat(ps, ")");
    }
}
