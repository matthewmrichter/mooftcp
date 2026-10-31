/* test_cfg_build.c - Plan 3 Task 6: the prefs writer (mt_cfg_build), the flags byte, the loader's
 * address/mask rulings and the preservation helpers the control panel uses (mac/glue/mt_cfg.h). */
#include "t.h"
#include "mt_cfg.h"

static void fill(mt_ifconfig *c, const char *dom) {
    memset(c, 0, sizeof *c);
    c->addr = 0xC0A80107u; c->mask = 0xFFFFFF00u; c->gateway = 0xC0A80101u; c->dns = 0x08080808u;
    c->mtu = 1500;
    if (dom) strcpy(c->domain, dom);
}

static void build_parse_round_trip(void) {
    static const int lens[] = { 0, 1, 63 };
    uint8_t buf[256], flags;
    mt_ifconfig a, b;
    uint8_t mode;
    char dom[65];
    size_t i;
    uint32_t n;
    for (i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        memset(dom, 'a', sizeof dom); dom[lens[i]] = 0;
        fill(&a, dom);
        n = mt_cfg_build(&a, MT_CFG_MODE_DHCP, 1, buf, sizeof buf);
        T_EQ(n, 22 + lens[i]);
        T_EQ(mt_cfg_parse_flags(buf, n, &b, &mode, &flags), 0);
        T_EQ(b.addr, a.addr); T_EQ(b.mask, a.mask); T_EQ(b.gateway, a.gateway); T_EQ(b.dns, a.dns);
        T_CHECK(strcmp(b.domain, dom) == 0);
        T_EQ(b.mtu, 1500);
        T_EQ(mode, MT_CFG_MODE_DHCP);
        T_EQ(flags, MT_CFG_FLAG_LOG);
        n = mt_cfg_build(&a, MT_CFG_MODE_MANUAL, 0, buf, sizeof buf);
        T_EQ(mt_cfg_parse_flags(buf, n, &b, &mode, &flags), 0);
        T_EQ(mode, MT_CFG_MODE_MANUAL);
        T_EQ(flags, 0);
    }
    /* byte layout, pinned */
    fill(&a, "x.y");
    n = mt_cfg_build(&a, 0, 1, buf, sizeof buf);
    { static const uint8_t want[] = { 0,1, 0,0, 192,168,1,7, 255,255,255,0, 192,168,1,1, 8,8,8,8,
                                      3,'x','.','y', 1 };
      T_EQ(n, sizeof want); T_MEMEQ(buf, want, sizeof want); }
    /* a 64-character domain (no terminator inside mt_ifconfig) cannot be written */
    memset(a.domain, 'a', 64);
    T_EQ(mt_cfg_build(&a, 0, 0, buf, sizeof buf), 0);
}

static void parse_truncated_at_every_length(void) {
    uint8_t buf[64], flags;
    mt_ifconfig a, b;
    uint8_t mode;
    uint32_t n, k;
    fill(&a, "ab.c");
    n = mt_cfg_build(&a, 1, 1, buf, sizeof buf);
    for (k = 0; k <= n; k++) {
        int r = mt_cfg_parse_flags(buf, k, &b, &mode, &flags);
        if (k < 2) { T_EQ(r, -1); T_EQ(b.addr, 0); T_EQ(mode, 0); T_EQ(flags, 0); continue; }
        T_EQ(r, 0);
        T_EQ(b.addr, k >= 8 ? a.addr : 0);
        T_EQ(b.mask, k >= 12 ? a.mask : (k >= 8 ? 0xFFFFFF00u : 0));   /* class C default */
        T_EQ(b.gateway, k >= 16 ? a.gateway : 0);
        T_EQ(b.dns, k >= 20 ? a.dns : 0);
        T_CHECK(k >= 25 ? strcmp(b.domain, "ab.c") == 0 : b.domain[0] == 0);
        T_EQ(flags, k >= 26 ? 1 : 0);
        T_EQ(mode, k >= 4 ? 1 : 0);
    }
}

static void parse_foreign_version_is_unconfigured(void) {
    uint8_t buf[64], flags = 9;
    mt_ifconfig a, b;
    uint8_t mode = 9;
    uint32_t n;
    fill(&a, "z");
    n = mt_cfg_build(&a, 1, 1, buf, sizeof buf);
    buf[1] = 2;
    T_EQ(mt_cfg_parse_flags(buf, n, &b, &mode, &flags), -1);
    T_EQ(b.addr, 0); T_EQ(b.domain[0], 0); T_EQ(mode, 0); T_EQ(flags, 0);
    buf[0] = 1; buf[1] = 1;
    T_EQ(mt_cfg_parse_flags(buf, n, &b, &mode, &flags), -1);
}

static void parse_trailing_unknown_fields_ignored(void) {
    uint8_t buf[64], flags;
    mt_ifconfig a, b;
    uint8_t mode;
    uint32_t n, tl;
    const uint8_t *t;
    static const uint8_t tail[] = { 0xDE, 0xAD, 0xBE, 0xEF, 7 };
    fill(&a, "q.r");
    n = mt_cfg_build_keep(&a, 0x0001, 0x81, tail, sizeof tail, buf, sizeof buf);
    T_EQ(n, 22 + 3 + sizeof tail);
    T_EQ(mt_cfg_parse_flags(buf, n, &b, &mode, &flags), 0);
    T_EQ(b.addr, a.addr); T_CHECK(strcmp(b.domain, "q.r") == 0);
    T_EQ(flags, 0x81);                                  /* unknown flag bits come through */
    t = mt_cfg_tail(buf, n, &tl);
    T_EQ(tl, sizeof tail); T_CHECK(t != NULL); T_MEMEQ(t, tail, sizeof tail);
    T_CHECK(mt_cfg_tail(buf, 22 + 3, &tl) == NULL); T_EQ(tl, 0);   /* flags, no tail */
    T_CHECK(mt_cfg_tail(buf, 21 + 3, &tl) == NULL); T_EQ(tl, 0);   /* no flags byte */
    T_CHECK(mt_cfg_tail(buf, 10, &tl) == NULL); T_EQ(tl, 0);
}

static void parse_flags_byte(void) {
    static const uint8_t none[] = { 0,1, 0,0, 10,0,0,2, 255,0,0,0, 0,0,0,0, 0,0,0,0, 0 };
    static const uint8_t on[]   = { 0,1, 0,0, 10,0,0,2, 255,0,0,0, 0,0,0,0, 0,0,0,0, 0, 1 };
    static const uint8_t other[] = { 0,1, 0,0, 10,0,0,2, 255,0,0,0, 0,0,0,0, 0,0,0,0, 0, 0xFE };
    static const uint8_t badlen[] = { 0,1, 0,0, 10,0,0,2, 255,0,0,0, 0,0,0,0, 0,0,0,0, 64, 1, 1 };
    mt_ifconfig c;
    uint8_t mode, f = 7;
    T_EQ(mt_cfg_parse_flags(none, sizeof none, &c, &mode, &f), 0); T_EQ(f, 0);
    T_EQ(mt_cfg_parse_flags(on, sizeof on, &c, &mode, &f), 0); T_EQ(f & MT_CFG_FLAG_LOG, 1);
    T_EQ(mt_cfg_parse_flags(other, sizeof other, &c, &mode, &f), 0); T_EQ(f & MT_CFG_FLAG_LOG, 0);
    T_EQ(f, 0xFE);
    T_EQ(mt_cfg_parse_flags(badlen, sizeof badlen, &c, &mode, &f), 0); T_EQ(f, 0);
    T_EQ(mt_cfg_parse_flags(on, sizeof on, &c, &mode, NULL), 0);          /* flags may be NULL */
    T_EQ(mt_cfg_parse(on, sizeof on, &c, &mode), 0); T_EQ(c.addr, 0x0A000002u);
}

static void build_cap_too_small(void) {
    uint8_t buf[64];
    mt_ifconfig a;
    uint32_t need, k;
    fill(&a, "host.example");
    need = mt_cfg_build(&a, 0, 0, buf, sizeof buf);
    T_EQ(need, 22 + 12);
    for (k = 0; k < need; k++) {
        memset(buf, 0xAA, sizeof buf);
        T_EQ(mt_cfg_build(&a, 0, 0, buf, k), 0);
        T_EQ(buf[0], 0xAA);                             /* nothing written on failure */
    }
    T_EQ(mt_cfg_build(&a, 0, 0, buf, need), need);
    T_EQ(mt_cfg_build(&a, 0, 0, NULL, 100), 0);
}

static uint32_t addr_of(uint32_t addr, uint32_t mask) {
    uint8_t r[12];
    mt_ifconfig c;
    uint8_t mode;
    r[0] = 0; r[1] = 1; r[2] = r[3] = 0;
    r[4] = (uint8_t)(addr >> 24); r[5] = (uint8_t)(addr >> 16); r[6] = (uint8_t)(addr >> 8); r[7] = (uint8_t)addr;
    r[8] = (uint8_t)(mask >> 24); r[9] = (uint8_t)(mask >> 16); r[10] = (uint8_t)(mask >> 8); r[11] = (uint8_t)mask;
    T_EQ(mt_cfg_parse(r, sizeof r, &c, &mode), 0);
    if (!c.addr) T_EQ(c.mask, 0);
    return c.addr;
}

static void addr_boundaries(void) {
    T_EQ(addr_of(0xDFFFFFFEu, 0), 0xDFFFFFFEu);         /* 223.255.255.254: accepted */
    T_EQ(addr_of(0xDFFFFFFFu, 0), 0);                   /* 223.255.255.255: class C broadcast */
    T_EQ(addr_of(0xE0000000u, 0), 0);                   /* 224.0.0.0: class D */
    T_EQ(addr_of(0xEFFFFFFFu, 0), 0);
    T_EQ(addr_of(0xF0000000u, 0), 0);                   /* class E */
    T_EQ(addr_of(0xFFFFFFFFu, 0), 0);                   /* limited broadcast */
    T_EQ(addr_of(0xFFFFFFFFu, 0xFFFFFFFFu), 0);
    T_EQ(addr_of(0x7F000001u, 0), 0);                   /* 127/8: Ruling, unconfigured */
    T_EQ(addr_of(0x7FFFFFFFu, 0), 0);
    T_EQ(addr_of(0x7E000001u, 0), 0x7E000001u);         /* 126.0.0.1: fine */
    T_EQ(addr_of(0x80000001u, 0), 0x80000001u);         /* 128.0.0.1: fine */
    T_EQ(addr_of(0x00000001u, 0), 0);                   /* 0.0.0.1 */
    T_EQ(addr_of(0x00FFFFFFu, 0), 0);
    T_EQ(addr_of(0x01000001u, 0), 0x01000001u);         /* 1.0.0.1 */
    T_EQ(addr_of(0x01000000u, 0), 0);                   /* 1.0.0.0: classful /8 network address */
    T_EQ(addr_of(0, 0), 0);
}

static void mask_rulings(void) {
    T_EQ(addr_of(0x0A000002u, 0xFF000000u), 0x0A000002u);
    T_EQ(addr_of(0x0A000002u, 0xFFFFFFFEu), 0x0A000002u);   /* /31 */
    T_EQ(addr_of(0x0A000002u, 0xFFFFFFFFu), 0x0A000002u);   /* /32 */
    T_EQ(addr_of(0x0A000002u, 0x80000000u), 0x0A000002u);   /* /1 */
    T_EQ(addr_of(0x0A000002u, 0xFF00FF00u), 0);             /* not contiguous */
    T_EQ(addr_of(0x0A000002u, 0xFFFFFFFDu), 0);
    T_EQ(addr_of(0x0A000002u, 0x00FFFFFFu), 0);
    T_EQ(addr_of(0x0A000002u, 0x7FFFFFFFu), 0);
    T_CHECK(!mt_cfg_mask_valid(0));
    T_CHECK(mt_cfg_mask_valid(0xFFFFFF00u));
    T_CHECK(mt_cfg_addr_valid(0xDFFFFFFEu, 0xFFFFFF00u));
    T_CHECK(!mt_cfg_addr_valid(0xE0000000u, 0xFFFFFF00u));
}

static void host_bits_rule(void) {
    uint8_t r[12];
    mt_ifconfig c;
    uint8_t mode;
    /* shared validator: host bits all 0 or all 1 are never ours when the mask is shorter than /31 */
    T_CHECK(!mt_cfg_addr_valid(0x0A000000u, 0xFFFFFF00u));   /* 10.0.0.0/24: network address */
    T_CHECK(!mt_cfg_addr_valid(0x0A0000FFu, 0xFFFFFF00u));   /* 10.0.0.255/24: broadcast */
    T_CHECK( mt_cfg_addr_valid(0x0A000001u, 0xFFFFFF00u));
    T_CHECK( mt_cfg_addr_valid(0x0A0000FEu, 0xFFFFFF00u));
    T_CHECK(!mt_cfg_addr_valid(0x0A000000u, 0xFF000000u));   /* 10.0.0.0/8 */
    T_CHECK(!mt_cfg_addr_valid(0x0AFFFFFFu, 0xFF000000u));
    T_CHECK(!mt_cfg_addr_valid(0x0A000000u, 0));             /* mask 0 = classful (/8) */
    T_CHECK( mt_cfg_addr_valid(0x0A000100u, 0xFF000000u));
    T_CHECK(!mt_cfg_addr_valid(0x0A000000u, 0xFFFFFFFCu));   /* /30: network */
    T_CHECK(!mt_cfg_addr_valid(0x0A000003u, 0xFFFFFFFCu));   /* /30: broadcast */
    T_CHECK( mt_cfg_addr_valid(0x0A000001u, 0xFFFFFFFCu));
    T_CHECK( mt_cfg_addr_valid(0x0A000000u, 0xFFFFFFFEu));   /* /31: both ends are hosts */
    T_CHECK( mt_cfg_addr_valid(0x0A000001u, 0xFFFFFFFEu));
    T_CHECK( mt_cfg_addr_valid(0x0A000000u, 0xFFFFFFFFu));   /* /32 */
    T_CHECK(!mt_cfg_addr_valid(0x7F000001u, 0xFF000000u));   /* the old rules still hold */
    T_CHECK(!mt_cfg_addr_valid(0x00000001u, 0xFF000000u));
    /* the loader applies it: unconfigured, never a silently dead network */
    r[0] = 0; r[1] = 1; r[2] = r[3] = 0; r[4] = 10; r[5] = r[6] = 0; r[7] = 255;
    r[8] = 255; r[9] = 255; r[10] = 255; r[11] = 0;
    T_EQ(mt_cfg_parse(r, sizeof r, &c, &mode), 0);
    T_EQ(c.addr, 0); T_EQ(c.mask, 0);
    r[7] = 0;
    T_EQ(mt_cfg_parse(r, sizeof r, &c, &mode), 0);
    T_EQ(c.addr, 0);
    r[7] = 1;
    T_EQ(mt_cfg_parse(r, sizeof r, &c, &mode), 0);
    T_EQ(c.addr, 0x0A000001u);
    r[8] = r[9] = r[10] = r[11] = 0;                          /* classful /8: 10.0.0.255 is fine */
    r[7] = 255;
    T_EQ(mt_cfg_parse(r, sizeof r, &c, &mode), 0);
    T_EQ(c.addr, 0x0A0000FFu);
    r[5] = r[6] = r[7] = 0;                                   /* 10.0.0.0 classful /8: network */
    T_EQ(mt_cfg_parse(r, sizeof r, &c, &mode), 0);
    T_EQ(c.addr, 0);
}

static void rewrite_preserves_unknowns(void) {
    /* what the control panel does: read, change the address, write back with the raw mode word,
     * the flags byte and the unknown tail */
    static const uint8_t old[] = { 0,1, 0,7, 10,0,0,2, 255,0,0,0, 0,0,0,0, 0,0,0,0, 2,'a','b', 0x41, 9,8,7 };
    uint8_t buf[64], flags, mode;
    mt_ifconfig c;
    uint32_t tl, n;
    const uint8_t *t;
    T_EQ(mt_cfg_parse_flags(old, sizeof old, &c, &mode, &flags), 0);
    T_EQ(mt_cfg_raw_mode(old, sizeof old), 7);
    T_EQ(mode, MT_CFG_MODE_MANUAL);                     /* folded for the driver */
    c.addr = 0x0A000003u;
    t = mt_cfg_tail(old, sizeof old, &tl);
    n = mt_cfg_build_keep(&c, mt_cfg_raw_mode(old, sizeof old), flags, t, tl, buf, sizeof buf);
    T_EQ(n, sizeof old);
    T_MEMEQ(buf, old, 4);
    T_MEMEQ(buf + 8, old + 8, sizeof old - 8);
    T_EQ(buf[7], 3);
    T_EQ(mt_cfg_raw_mode(NULL, 0), 0);
}

T_MAIN(build_parse_round_trip, parse_truncated_at_every_length, parse_foreign_version_is_unconfigured,
       parse_trailing_unknown_fields_ignored, parse_flags_byte, build_cap_too_small, addr_boundaries,
       mask_rulings, rewrite_preserves_unknowns, host_bits_rule)
