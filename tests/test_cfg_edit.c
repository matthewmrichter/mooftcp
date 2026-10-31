/* test_cfg_edit.c - Plan 3 Task 7: control panel field parsing, validation and prefs encoding
 * (mac/cdev/cfg_edit.[ch]; Review Focus 3). */
#include "t.h"
#include "cfg_edit.h"
#include "mt_cfg.h"

static void setf(CfeFields *f, const char *ip, const char *mask, const char *gw, const char *dns,
                 const char *dom) {
    memset(f, 0, sizeof *f);
    strcpy(f->ip, ip); strcpy(f->mask, mask); strcpy(f->gw, gw); strcpy(f->dns, dns);
    strcpy(f->domain, dom);
}

static int val(const char *ip, const char *mask, const char *gw, const char *dns, const char *dom) {
    CfeFields f;
    CfeError e;
    int rc;
    setf(&f, ip, mask, gw, dns, dom);
    rc = cfe_validate(&f, &e);
    T_EQ(e.code, rc);
    return rc;
}

static void parse_ip_table(void) {
    static const struct { const char *s; int ok; mt_ip4 v; } t[] = {
        { "1.2.3.4", 1, 0x01020304u }, { "0.0.0.0", 1, 0 }, { "255.255.255.255", 1, 0xFFFFFFFFu },
        { "10.0.0.1", 1, 0x0A000001u }, { "192.168.100.200", 1, 0xC0A864C8u },
        { "0.10.100.0", 1, 0x000A6400u },
        { "256.1.1.1", 0, 0 }, { "1.2.3", 0, 0 }, { "1.2.3.4.5", 0, 0 }, { " 1.2.3.4", 0, 0 },
        { "1.2.3.4 ", 0, 0 }, { "01.2.3.4", 0, 0 }, { "1.2.3.04", 0, 0 }, { "1.00.3.4", 0, 0 },
        { "1..3.4", 0, 0 }, { ".1.2.3", 0, 0 }, { "1.2.3.", 0, 0 }, { "", 0, 0 },
        { "1.2.3.4a", 0, 0 }, { "a.b.c.d", 0, 0 }, { "1,2,3,4", 0, 0 }, { "-1.2.3.4", 0, 0 },
        { "+1.2.3.4", 0, 0 }, { "1.2.3.1000", 0, 0 }, { "1.2.3.256", 0, 0 }, { "0x1.2.3.4", 0, 0 },
        { "1. 2.3.4", 0, 0 }, { "1.2.3.4\n", 0, 0 }, { "00.0.0.0", 0, 0 }, { "999.1.1.1", 0, 0 },
    };
    size_t i;
    mt_ip4 v;
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        v = 0xDEADBEEFu;
        T_EQ(cfe_parse_ip(t[i].s, &v), t[i].ok);
        if (t[i].ok) T_EQ(v, t[i].v); else T_EQ(v, 0xDEADBEEFu);
    }
    T_EQ(cfe_parse_ip(NULL, &v), 0);
}

static void parse_mask_table(void) {
    static const struct { const char *s; int ok; mt_ip4 v; } t[] = {
        { "255.255.255.0", 1, 0xFFFFFF00u }, { "255.0.0.0", 1, 0xFF000000u },
        { "255.255.255.255", 1, 0xFFFFFFFFu }, { "255.255.255.254", 1, 0xFFFFFFFEu },
        { "128.0.0.0", 1, 0x80000000u }, { "255.255.252.0", 1, 0xFFFFFC00u },
        { "255.255.0.255", 0, 0 }, { "0.0.0.0", 0, 0 }, { "255.0.255.0", 0, 0 },
        { "0.255.255.255", 0, 0 }, { "255.255.255.253", 0, 0 }, { "127.0.0.0", 0, 0 },
        { "255.255.255", 0, 0 }, { "255.255.255.256", 0, 0 }, { "", 0, 0 }, { "255.255.255.00", 0, 0 },
    };
    size_t i;
    mt_ip4 v;
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        v = 0xDEADBEEFu;
        T_EQ(cfe_parse_mask(t[i].s, &v), t[i].ok);
        if (t[i].ok) T_EQ(v, t[i].v); else T_EQ(v, 0xDEADBEEFu);
    }
}

static void format_round_trip(void) {
    static const mt_ip4 t[] = { 0, 1, 0x0A000001u, 0xC0A80101u, 0xFFFFFFFFu, 0x01020304u, 0x64646464u };
    size_t i;
    char b[16];
    mt_ip4 v;
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        cfe_format_ip(t[i], b);
        T_CHECK(strlen(b) <= 15);
        T_EQ(cfe_parse_ip(b, &v), 1);
        T_EQ(v, t[i]);
    }
    cfe_format_ip(0xC0A80107u, b);
    T_CHECK(strcmp(b, "192.168.1.7") == 0);
}

static void validate_empty_and_basic(void) {
    CfeFields f;
    CfeError e;
    memset(&f, 0, sizeof f);
    e.field = 9; e.code = 9;
    T_EQ(cfe_validate(&f, &e), CFE_OK);                 /* the stack stays down */
    T_EQ(e.field, CFE_F_NONE); T_EQ(e.code, 0);
    T_EQ(cfe_validate(&f, NULL), CFE_OK);               /* err is optional */
    T_EQ(val("", "", "", "", "example.com"), CFE_OK);   /* a domain alone */
    T_EQ(val("192.168.1.7", "", "", "", ""), CFE_OK);   /* classful */
    T_EQ(val("192.168.1.7", "255.255.255.0", "192.168.1.1", "8.8.8.8", "lan"), CFE_OK);
    T_EQ(val("10.0.0.2", "255.0.0.0", "10.0.0.1", "", ""), CFE_OK);
    T_EQ(val("192.168.1.7", "255.255.255.0", "", "8.8.8.8", ""), CFE_OK);
    setf(&f, "1.2.3", "", "", "", "");
    T_EQ(cfe_validate(&f, &e), CFE_E_IP); T_EQ(e.field, CFE_F_IP);
    setf(&f, "10.0.0.2", "255.255.0.255", "", "", "");
    T_EQ(cfe_validate(&f, &e), CFE_E_MASK); T_EQ(e.field, CFE_F_MASK);
    setf(&f, "10.0.0.2", "0.0.0.0", "", "", "");
    T_EQ(cfe_validate(&f, &e), CFE_E_MASK);
    setf(&f, "10.0.0.2", "255.255.255.255", "", "", "");
    T_EQ(cfe_validate(&f, &e), CFE_OK);                 /* /32 */
    setf(&f, "10.0.0.2", "", "1.2", "", "");
    T_EQ(cfe_validate(&f, &e), CFE_E_GW); T_EQ(e.field, CFE_F_GW);
    setf(&f, "10.0.0.2", "", "", "8.8.8", "");
    T_EQ(cfe_validate(&f, &e), CFE_E_DNS); T_EQ(e.field, CFE_F_DNS);
    setf(&f, "10.0.0.2", "", "", "08.8.8.8", "");
    T_EQ(cfe_validate(&f, &e), CFE_E_DNS);
    setf(&f, " 10.0.0.2", "", "", "", "");
    T_EQ(cfe_validate(&f, &e), CFE_E_IP);
}

static void validate_reserved_addresses(void) {
    CfeFields f;
    CfeError e;
    T_EQ(val("0.0.0.0", "", "", "", ""), CFE_E_ADDR_RESERVED);   /* empty means unconfigured */
    T_EQ(val("255.255.255.255", "", "", "", ""), CFE_E_ADDR_RESERVED);
    T_EQ(val("224.0.0.1", "", "", "", ""), CFE_E_ADDR_RESERVED);
    T_EQ(val("239.1.2.3", "", "", "", ""), CFE_E_ADDR_RESERVED);
    T_EQ(val("240.0.0.1", "", "", "", ""), CFE_E_ADDR_RESERVED);
    T_EQ(val("127.0.0.1", "", "", "", ""), CFE_E_ADDR_RESERVED);
    T_EQ(val("127.1.2.3", "255.255.255.0", "", "", ""), CFE_E_ADDR_RESERVED);
    T_EQ(val("0.1.2.3", "", "", "", ""), CFE_E_ADDR_RESERVED);
    T_EQ(val("223.255.255.254", "", "", "", ""), CFE_OK);
    T_EQ(val("126.0.0.1", "", "", "", ""), CFE_OK);
    T_EQ(val("1.0.0.1", "", "", "", ""), CFE_OK);
    /* gateway and DNS */
    T_EQ(val("192.168.1.7", "255.255.255.0", "0.0.0.0", "", ""), CFE_E_ADDR_RESERVED);
    T_EQ(val("192.168.1.7", "255.255.255.0", "127.0.0.1", "", ""), CFE_E_ADDR_RESERVED);
    T_EQ(val("192.168.1.7", "255.255.255.0", "224.0.0.1", "", ""), CFE_E_ADDR_RESERVED);
    T_EQ(val("192.168.1.7", "255.255.255.0", "255.255.255.255", "", ""), CFE_E_ADDR_RESERVED);
    T_EQ(val("192.168.1.7", "255.255.255.0", "", "0.0.0.0", ""), CFE_E_ADDR_RESERVED);
    T_EQ(val("192.168.1.7", "255.255.255.0", "", "127.0.0.53", ""), CFE_E_ADDR_RESERVED);
    T_EQ(val("192.168.1.7", "255.255.255.0", "", "224.0.0.251", ""), CFE_E_ADDR_RESERVED);
    T_EQ(val("192.168.1.7", "255.255.255.0", "", "0.1.1.1", ""), CFE_E_ADDR_RESERVED);
    setf(&f, "192.168.1.7", "255.255.255.0", "", "255.1.1.1", "");
    T_EQ(cfe_validate(&f, &e), CFE_E_ADDR_RESERVED); T_EQ(e.field, CFE_F_DNS);
    /* gateway equal to us, or our subnet's network/broadcast */
    setf(&f, "192.168.1.7", "255.255.255.0", "192.168.1.7", "", "");
    T_EQ(cfe_validate(&f, &e), CFE_E_GW_SELF); T_EQ(e.field, CFE_F_GW);
    T_EQ(val("192.168.1.7", "255.255.255.0", "192.168.1.0", "", ""), CFE_E_ADDR_RESERVED);
    T_EQ(val("192.168.1.7", "255.255.255.0", "192.168.1.255", "", ""), CFE_E_ADDR_RESERVED);
    T_EQ(val("192.168.1.7", "255.255.255.254", "192.168.1.6", "", ""), CFE_OK);   /* /31 */
}

static void validate_host_bits(void) {
    CfeFields f;
    CfeError e;
    setf(&f, "10.0.0.255", "255.255.255.0", "", "", "");
    T_EQ(cfe_validate(&f, &e), CFE_E_HOSTBITS); T_EQ(e.field, CFE_F_IP);
    T_EQ(val("10.0.0.0", "255.255.255.0", "", "", ""), CFE_E_HOSTBITS);
    T_EQ(val("10.255.255.255", "255.0.0.0", "", "", ""), CFE_E_HOSTBITS);
    T_EQ(val("10.0.0.0", "", "", "", ""), CFE_E_HOSTBITS);           /* classful /8 */
    T_EQ(val("172.16.255.255", "", "", "", ""), CFE_E_HOSTBITS);     /* classful /16 */
    T_EQ(val("192.168.1.255", "", "", "", ""), CFE_E_HOSTBITS);      /* classful /24 */
    T_EQ(val("10.0.0.255", "", "", "", ""), CFE_OK);                 /* classful /8: a host */
    T_EQ(val("10.0.0.255", "255.255.0.0", "", "", ""), CFE_OK);
    T_EQ(val("10.0.0.0", "255.255.0.0", "", "", ""), CFE_E_HOSTBITS);
    T_EQ(val("10.0.1.0", "255.255.0.0", "", "", ""), CFE_OK);
    T_EQ(val("10.0.0.0", "255.255.255.254", "", "", ""), CFE_OK);    /* /31 */
    T_EQ(val("10.0.0.1", "255.255.255.254", "", "", ""), CFE_OK);
    T_EQ(val("10.0.0.0", "255.255.255.255", "", "", ""), CFE_OK);    /* /32 */
    T_EQ(val("10.0.0.3", "255.255.255.252", "", "", ""), CFE_E_HOSTBITS);
    T_EQ(val("10.0.0.1", "255.255.255.252", "", "", ""), CFE_OK);
}

static void validate_ip_required(void) {
    CfeFields f;
    CfeError e;
    /* mask, gateway or DNS typed with no address: an error, never silently ignored */
    setf(&f, "", "255.255.255.0", "", "", "");
    T_EQ(cfe_validate(&f, &e), CFE_E_IP); T_EQ(e.field, CFE_F_IP);
    T_EQ(val("", "", "192.168.1.1", "", ""), CFE_E_IP);
    T_EQ(val("", "", "", "8.8.8.8", ""), CFE_E_IP);
    T_EQ(val("", "", "", "8.8.8.8", "example.com"), CFE_E_IP);
    /* a syntax error in a field is reported before the missing address */
    setf(&f, "", "255.255.0.255", "", "", "");
    T_EQ(cfe_validate(&f, &e), CFE_E_MASK);
}

static void validate_gateway_offnet_warning(void) {
    CfeFields f;
    CfeError e;
    int rc;
    setf(&f, "192.168.1.7", "255.255.255.0", "192.168.2.1", "", "");
    rc = cfe_validate(&f, &e);
    T_EQ(rc, CFE_E_GW_OFFNET); T_EQ(e.code, CFE_E_GW_OFFNET); T_EQ(e.field, CFE_F_GW);
    T_CHECK(!cfe_is_fatal(rc));
    T_CHECK(cfe_is_fatal(CFE_E_IP)); T_CHECK(cfe_is_fatal(CFE_E_GW_SELF));
    T_CHECK(!cfe_is_fatal(CFE_OK));
    { uint8_t buf[100];                              /* the warning does not block encoding */
      T_CHECK(cfe_encode(&f, NULL, 0, buf, sizeof buf) > 0); }
    /* classful: 10.0.0.2 and 11.0.0.1 differ under /8 */
    T_EQ(val("10.0.0.2", "", "11.0.0.1", "", ""), CFE_E_GW_OFFNET);
    T_EQ(val("10.0.0.2", "", "10.9.9.9", "", ""), CFE_OK);
    /* a fatal error wins over the warning */
    T_EQ(val("192.168.1.7", "255.255.255.0", "192.168.2.1", "224.0.0.1", ""), CFE_E_ADDR_RESERVED);
    T_EQ(val("192.168.1.7", "255.255.255.0", "192.168.2.1", "8.8.8", ""), CFE_E_DNS);
    T_EQ(val("192.168.1.7", "255.255.255.0", "192.168.2.1", "", "bad domain"), CFE_E_DOMAIN);
    /* a reserved gateway is fatal, not off-net */
    T_EQ(val("192.168.1.7", "255.255.255.0", "127.0.0.1", "", ""), CFE_E_ADDR_RESERVED);
    /* /32: any other gateway is off-net */
    T_EQ(val("10.0.0.2", "255.255.255.255", "10.0.0.1", "", ""), CFE_E_GW_OFFNET);
}

static void validate_domain(void) {
    CfeFields f;
    CfeError e;
    char d[80];
    T_EQ(val("", "", "", "", "a"), CFE_OK);
    T_EQ(val("", "", "", "", "my-host_1.Example.COM"), CFE_OK);
    memset(d, 'a', 63); d[63] = 0;
    T_EQ(val("", "", "", "", d), CFE_OK);               /* 63 is the limit */
    memset(d, 'a', 64); d[64] = 0;
    setf(&f, "", "", "", "", "");
    memcpy(f.domain, d, 63); f.domain[63] = 'a'; f.domain[63] = 0;
    T_EQ(cfe_validate(&f, &e), CFE_OK);
    memset(f.domain, 'a', 64);                          /* 64 chars, no terminator in the buffer */
    T_EQ(cfe_validate(&f, &e), CFE_E_DOMAIN); T_EQ(e.field, CFE_F_DOMAIN);
    T_EQ(val("", "", "", "", ".example.com"), CFE_E_DOMAIN);
    T_EQ(val("", "", "", "", "example.com."), CFE_E_DOMAIN);
    T_EQ(val("", "", "", "", "a..b"), CFE_E_DOMAIN);
    T_EQ(val("", "", "", "", "."), CFE_E_DOMAIN);
    T_EQ(val("", "", "", "", "exa mple"), CFE_E_DOMAIN);
    T_EQ(val("", "", "", "", " example"), CFE_E_DOMAIN);
    T_EQ(val("", "", "", "", "ex@mple"), CFE_E_DOMAIN);
    T_EQ(val("", "", "", "", "ex/mple"), CFE_E_DOMAIN);
    T_EQ(val("", "", "", "", "caf\xC3\xA9"), CFE_E_DOMAIN);
    /* an unterminated IP buffer is invalid, not an overread */
    memset(&f, 0, sizeof f);
    memset(f.ip, '1', sizeof f.ip);
    T_EQ(cfe_validate(&f, &e), CFE_E_IP);
    memset(&f, 0, sizeof f);
    memset(f.dns, '1', sizeof f.dns);
    T_EQ(cfe_validate(&f, &e), CFE_E_DNS);
}

static void encode_decode_round_trip(void) {
    static const struct { const char *ip, *mask, *gw, *dns, *dom; } t[] = {
        { "", "", "", "", "" },
        { "", "", "", "", "example.com" },
        { "192.168.1.7", "255.255.255.0", "192.168.1.1", "8.8.8.8", "lan" },
        { "10.0.0.2", "255.0.0.0", "10.0.0.1", "", "" },
        { "172.16.5.9", "255.255.0.0", "", "1.1.1.1", "a.b-c_d" },
        { "10.0.0.2", "255.255.255.255", "", "", "" },
    };
    uint8_t buf[128];
    CfeFields a, b;
    size_t i;
    uint32_t n;
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        setf(&a, t[i].ip, t[i].mask, t[i].gw, t[i].dns, t[i].dom);
        n = cfe_encode(&a, NULL, 0, buf, sizeof buf);
        T_EQ(n, 22 + strlen(t[i].dom));
        T_EQ(cfe_decode(buf, n, &b), 0);
        T_CHECK(strcmp(b.ip, a.ip) == 0); T_CHECK(strcmp(b.mask, a.mask) == 0);
        T_CHECK(strcmp(b.gw, a.gw) == 0); T_CHECK(strcmp(b.dns, a.dns) == 0);
        T_CHECK(strcmp(b.domain, a.domain) == 0);
        /* a new file: mode manual, flags 0 */
        T_EQ(mt_cfg_raw_mode(buf, n), 0);
        T_EQ(buf[n - 1], 0);
    }
    /* the loader sees what the panel wrote */
    { mt_ifconfig c; uint8_t mode, fl;
      setf(&a, "192.168.1.7", "255.255.255.0", "192.168.1.1", "8.8.8.8", "lan");
      n = cfe_encode(&a, NULL, 0, buf, sizeof buf);
      T_EQ(mt_cfg_parse_flags(buf, n, &c, &mode, &fl), 0);
      T_EQ(c.addr, 0xC0A80107u); T_EQ(c.mask, 0xFFFFFF00u); T_EQ(c.gateway, 0xC0A80101u);
      T_EQ(c.dns, 0x08080808u); T_CHECK(strcmp(c.domain, "lan") == 0); }
    /* a classful mask stays 0 on the wire */
    setf(&a, "192.168.1.7", "", "", "", "");
    n = cfe_encode(&a, NULL, 0, buf, sizeof buf);
    T_EQ(mt_cfg_rd32(buf + 8), 0);
    T_EQ(cfe_decode(buf, n, &b), 0);
    T_CHECK(strcmp(b.mask, "255.255.255.0") == 0);      /* decode shows the effective mask */
}

static void encode_refuses_fatal_and_small_cap(void) {
    CfeFields f;
    uint8_t buf[64];
    uint32_t n;
    memset(buf, 0xAA, sizeof buf);
    setf(&f, "10.0.0.255", "255.255.255.0", "", "", "");
    T_EQ(cfe_encode(&f, NULL, 0, buf, sizeof buf), 0);
    T_EQ(buf[0], 0xAA);                                 /* nothing written */
    setf(&f, "256.1.1.1", "", "", "", "");
    T_EQ(cfe_encode(&f, NULL, 0, buf, sizeof buf), 0);
    setf(&f, "10.0.0.2", "", "", "", "ab");
    n = cfe_encode(&f, NULL, 0, buf, sizeof buf);
    T_EQ(n, 24);
    T_EQ(cfe_encode(&f, NULL, 0, buf, n - 1), 0);
    T_EQ(cfe_encode(&f, NULL, 0, NULL, 100), 0);
}

static void encode_keeps_prior_mode_flags_tail(void) {
    /* DHCP mode word 7 (unknown), flags 0x81, tail DE AD, an old IP and domain */
    static const uint8_t prior[] = { 0,1, 0,7, 10,0,0,2, 255,0,0,0, 0,0,0,0, 0,0,0,0, 2,'a','b',
                                     0x81, 0xDE, 0xAD };
    uint8_t buf[128];
    CfeFields f, g;
    uint32_t n;
    setf(&f, "192.168.1.7", "255.255.255.0", "192.168.1.1", "8.8.8.8", "new.example");
    n = cfe_encode(&f, prior, sizeof prior, buf, sizeof buf);
    T_EQ(n, 22 + 11 + 2);
    T_EQ(mt_cfg_raw_mode(buf, n), 7);                   /* raw mode word, not folded */
    T_EQ(buf[20 + 1 + 11], 0x81);                       /* flags byte incl. unknown bits */
    T_EQ(buf[n - 2], 0xDE); T_EQ(buf[n - 1], 0xAD);
    T_EQ(cfe_decode(buf, n, &g), 0);
    T_CHECK(strcmp(g.ip, "192.168.1.7") == 0);
    T_CHECK(strcmp(g.domain, "new.example") == 0);
    /* prior without flags byte / tail: flags 0, no tail, mode kept */
    { static const uint8_t p2[] = { 0,1, 0,1, 10,0,0,2, 255,0,0,0, 0,0,0,0, 0,0,0,0, 0 };
      n = cfe_encode(&f, p2, sizeof p2, buf, sizeof buf);
      T_EQ(n, 22 + 11); T_EQ(mt_cfg_raw_mode(buf, n), 1); T_EQ(buf[n - 1], 0); }
    /* a prior that is shorter than the version word, or only the version word */
    { static const uint8_t p3[] = { 0,1 };
      n = cfe_encode(&f, p3, sizeof p3, buf, sizeof buf);
      T_EQ(n, 22 + 11); T_EQ(mt_cfg_raw_mode(buf, n), 0); }
    /* unusable prior: foreign version, corrupt, empty, wrong length: treated as none */
    { uint8_t bad[sizeof prior];
      uint32_t k;
      memcpy(bad, prior, sizeof prior);
      bad[1] = 2;                                       /* foreign version */
      n = cfe_encode(&f, bad, sizeof bad, buf, sizeof buf);
      T_EQ(n, 22 + 11); T_EQ(mt_cfg_raw_mode(buf, n), 0); T_EQ(buf[n - 1], 0);
      memcpy(bad, prior, sizeof prior);
      bad[0] = 0xFF;                                    /* corrupt version */
      n = cfe_encode(&f, bad, sizeof bad, buf, sizeof buf);
      T_EQ(n, 22 + 11); T_EQ(mt_cfg_raw_mode(buf, n), 0);
      n = cfe_encode(&f, prior, 0, buf, sizeof buf);    /* zero length */
      T_EQ(n, 22 + 11); T_EQ(mt_cfg_raw_mode(buf, n), 0);
      n = cfe_encode(&f, prior, 1, buf, sizeof buf);
      T_EQ(n, 22 + 11); T_EQ(mt_cfg_raw_mode(buf, n), 0);
      memcpy(bad, prior, sizeof prior);
      bad[20] = 200;                                    /* domain length runs past the end */
      n = cfe_encode(&f, bad, sizeof bad, buf, sizeof buf);
      T_EQ(n, 22 + 11); T_EQ(buf[n - 1], 0);           /* flags/tail not trusted */
      T_EQ(mt_cfg_raw_mode(buf, n), 7);                 /* version is fine, mode word is kept */
      for (k = 0; k <= sizeof prior; k++) {             /* every truncation: never overruns */
          n = cfe_encode(&f, prior, k, buf, sizeof buf);
          T_CHECK(n >= 33 && n <= 35);
      } }
}

static void encode_rewrite_stable(void) {
    /* decode, re-encode with the same prior: the bytes come back unchanged for a clean resource */
    static const uint8_t prior[] = { 0,1, 0,7, 192,168,1,7, 255,255,255,0, 192,168,1,1, 8,8,8,8,
                                     3,'l','a','n', 0x01, 1,2,3 };
    uint8_t buf[64];
    CfeFields f;
    uint32_t n;
    T_EQ(cfe_decode(prior, sizeof prior, &f), 0);
    n = cfe_encode(&f, prior, sizeof prior, buf, sizeof buf);
    T_EQ(n, sizeof prior);
    T_MEMEQ(buf, prior, sizeof prior);
}

static void decode_blanks_invalid(void) {
    CfeFields f;
    static const uint8_t bad_ip[] = { 0,1, 0,0, 127,0,0,1, 255,0,0,0, 127,0,0,1, 224,0,0,1, 0 };
    static const uint8_t bad_mask[] = { 0,1, 0,0, 10,0,0,2, 255,0,255,0, 10,0,0,1, 8,8,8,8, 0 };
    static const uint8_t hostbits[] = { 0,1, 0,0, 10,0,0,255, 255,255,255,0, 10,0,0,1, 0,0,0,0, 0 };
    static const uint8_t bad_dom[] = { 0,1, 0,0, 10,0,0,2, 0,0,0,0, 0,0,0,0, 0,0,0,0, 3,'a',' ','b' };
    static const uint8_t other_ver[] = { 0,2, 0,0, 10,0,0,2 };
    memset(&f, 'x', sizeof f);
    T_EQ(cfe_decode(other_ver, sizeof other_ver, &f), -1);
    T_EQ(f.ip[0], 0); T_EQ(f.domain[0], 0);              /* all empty */
    T_EQ(cfe_decode(NULL, 0, &f), -1);
    T_EQ(cfe_decode(bad_ip, sizeof bad_ip, &f), 0);
    T_EQ(f.ip[0], 0); T_EQ(f.mask[0], 0); T_EQ(f.gw[0], 0); T_EQ(f.dns[0], 0);
    T_EQ(cfe_decode(bad_mask, sizeof bad_mask, &f), 0);
    T_EQ(f.ip[0], 0); T_EQ(f.mask[0], 0);
    T_CHECK(strcmp(f.gw, "10.0.0.1") == 0); T_CHECK(strcmp(f.dns, "8.8.8.8") == 0);
    T_EQ(cfe_decode(hostbits, sizeof hostbits, &f), 0);
    T_EQ(f.ip[0], 0);                                     /* the loader refuses it too */
    T_EQ(cfe_decode(bad_dom, sizeof bad_dom, &f), 0);
    T_CHECK(strcmp(f.ip, "10.0.0.2") == 0); T_EQ(f.domain[0], 0);
    /* the shared validator and the loader agree on every address in a sweep */
    { uint32_t a, masks[3] = { 0xFFFFFF00u, 0xFFFFFFFCu, 0xFF000000u };
      int m;
      for (m = 0; m < 3; m++)
          for (a = 0x0A000000u; a < 0x0A000200u; a++) {
              uint8_t r[12]; mt_ifconfig c; uint8_t md; CfeFields g; char ip[16], mk[16];
              r[0] = 0; r[1] = 1; r[2] = r[3] = 0;
              r[4] = (uint8_t)(a >> 24); r[5] = (uint8_t)(a >> 16); r[6] = (uint8_t)(a >> 8); r[7] = (uint8_t)a;
              r[8] = (uint8_t)(masks[m] >> 24); r[9] = (uint8_t)(masks[m] >> 16);
              r[10] = (uint8_t)(masks[m] >> 8); r[11] = (uint8_t)masks[m];
              T_EQ(mt_cfg_parse(r, sizeof r, &c, &md), 0);
              cfe_format_ip(a, ip); cfe_format_ip(masks[m], mk);
              setf(&g, ip, mk, "", "", "");
              T_EQ(c.addr != 0, !cfe_is_fatal(cfe_validate(&g, NULL)));
          } }
}

/* Fuzz-ish: random strings over a small alphabet. Never crashes; accepts only strings that
 * re-render identically (canonical form), which also means no spaces, no leading zeros. */
static void parse_ip_idempotent_fuzz(void) {
    static const char alpha[] = "0123456789. a-";
    uint32_t seed = 12345u;
    int iter, accepted = 0;
    char s[48], b[16];
    mt_ip4 v, w;
    for (iter = 0; iter < 60000; iter++) {
        uint32_t len, i;
        seed = seed * 1664525u + 1013904223u;
        len = (seed >> 16) % 41u;
        for (i = 0; i < len; i++) {
            seed = seed * 1664525u + 1013904223u;
            /* bias toward digits and dots so some strings are valid */
            s[i] = (seed >> 24) % 4u ? alpha[(seed >> 8) % 11u] : alpha[(seed >> 8) % 14u];
        }
        s[len] = 0;
        if (cfe_parse_ip(s, &v)) {
            accepted++;
            cfe_format_ip(v, b);
            T_CHECK(strcmp(b, s) == 0);
            T_EQ(cfe_parse_ip(b, &w), 1);
            T_EQ(w, v);
        }
        if (cfe_parse_mask(s, &w)) {
            T_EQ(cfe_parse_ip(s, &v), 1);
            T_EQ(w, v);
            T_CHECK(mt_cfg_mask_valid(w));
        }
    }
    T_CHECK(accepted > 0);
    /* the exhaustive tail: every canonical rendering of boundary values parses */
    for (iter = 0; iter < 256; iter++) {
        char t[20];
        sprintf(t, "%d.%d.%d.%d", iter, 255 - iter, iter / 2, iter);
        T_EQ(cfe_parse_ip(t, &v), 1);
        cfe_format_ip(v, b);
        T_CHECK(strcmp(b, t) == 0);
    }
}

T_MAIN(parse_ip_table, parse_mask_table, format_round_trip, validate_empty_and_basic,
       validate_reserved_addresses, validate_host_bits, validate_ip_required,
       validate_gateway_offnet_warning, validate_domain, encode_decode_round_trip,
       encode_refuses_fatal_and_small_cap, encode_keeps_prior_mode_flags_tail, encode_rewrite_stable,
       decode_blanks_invalid, parse_ip_idempotent_fuzz)
