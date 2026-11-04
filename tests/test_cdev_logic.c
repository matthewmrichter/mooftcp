/* test_cdev_logic.c - Plan 3 Task 8 fix round 1: the control panel's pure parts
 * (mac/cdev/cdev_logic.[ch]) and the unit-table walk (mac/include/mt_units.h). */
#include "t.h"
#include "cdev_logic.h"
#include "mt_units.h"

static void setf(CfeFields *f, const char *ip, const char *mask, const char *gw, const char *dns,
                 const char *dom) {
    memset(f, 0, sizeof *f);
    strcpy(f->ip, ip); strcpy(f->mask, mask); strcpy(f->gw, gw); strcpy(f->dns, dns);
    strcpy(f->domain, dom);
}

static int pstr_is(const unsigned char *ps, const char *c) {
    return ps[0] == strlen(c) && memcmp(ps + 1, c, ps[0]) == 0;
}

static int pstr_has(const unsigned char *ps, const char *c) {
    char b[256];
    memcpy(b, ps + 1, ps[0]);
    b[ps[0]] = 0;
    return strstr(b, c) != NULL;
}

/* The DRVR header + stubs as moof.r emits them: the hex strings of resource 'DRVR' (22) up to the
 * first one that ends the first field ('",'). A change of the header or the open stub there
 * breaks this test. */
static int moof_drvr_header(uint8_t *out, int cap) {
    char path[512], line[256];
    FILE *fp;
    int in = 0, n = 0, done = 0;
    snprintf(path, sizeof path, "%s/mac/moof/moof.r", MT_SOURCE_DIR);
    fp = fopen(path, "r");
    if (!fp) return -1;
    while (!done && fgets(line, sizeof line, fp)) {
        char *p, *q;
        if (!in) { if (strstr(line, "resource 'DRVR' (22")) in = 1; continue; }
        p = strstr(line, "$\"");
        if (!p) continue;
        p += 2;
        q = strchr(p, '"');
        if (!q) continue;
        if (q[1] == ',') done = 1;
        for (; p < q; p++) {
            unsigned v;
            if (*p == ' ') continue;
            if (sscanf(p, "%2x", &v) != 1 || n >= cap) { fclose(fp); return -1; }
            out[n++] = (uint8_t)v;
            p++;
        }
    }
    fclose(fp);
    return done ? n : -1;
}

static void units_is_moof_matches_moof_r(void) {
    uint8_t h[128];
    static const uint8_t mactcp[] = { 0x5C, 0x00, 0, 0, 0, 0, 0, 0, 0x00, 0x32, 0x00, 0x36, 0x00, 0x3A,
                                      0x00, 0x3E, 0x00, 0x42, 0x04, '.', 'i', 'p', 'p', 0,
                                      0, 0, 0, 0, 0, 0, 0, 0 };
    int n = moof_drvr_header(h, (int)sizeof h);
    T_EQ(n, 48);
    T_EQ(h[18], 4);
    T_CHECK(memcmp(h + 19, ".ipp", 4) == 0);
    T_EQ(mt_units_is_moof(h), 1);
    h[25] = 0x01;                                   /* another open stub */
    T_EQ(mt_units_is_moof(h), 0);
    T_EQ(mt_units_is_moof(mactcp), 0);              /* MacTCP's header: open entry at 0x32 */
    T_EQ(mt_units_is_moof(NULL), 0);
}

/* Fake unit table: DCEs with a native dCtlDriver pointer, the flags word right after it. */
typedef struct { uint8_t b[2 * sizeof(void *) + 8]; } FakeDce;

static void dce_set(FakeDce *d, const void *drv, int ram) {
    memset(d, 0, sizeof *d);
    memcpy(d->b, &drv, sizeof drv);
    d->b[sizeof(void *) + 1] = ram ? 0x40 : 0x00;
}

static void drv_set(uint8_t *drv, const char *name) {
    memset(drv, 0, 64);
    drv[18] = (uint8_t)strlen(name);
    memcpy(drv + 19, name, strlen(name));
}

static void units_find_ipp_fake_table(void) {
    static uint8_t enet[64], rom_ipp[64], ram_ipp[64], other[64];
    FakeDce d_enet, d_rom, d_ram, d_purged, d_other;
    uint8_t *p_enet, *p_rom, *p_ram, *p_purged, *p_other;
    uint8_t **h_enet, **h_rom, **h_ram, **h_purged, **h_other;
    uint8_t *ram_master = ram_ipp, *purged_master = NULL;
    uint8_t **tbl[8];
    uintptr_t all = ~(uintptr_t)0;

    drv_set(enet, ".ENET0");
    drv_set(rom_ipp, ".IPP");                       /* any case */
    drv_set(ram_ipp, ".ipp");
    drv_set(other, ".ipq");
    dce_set(&d_enet, enet, 0);
    dce_set(&d_rom, rom_ipp, 0);
    dce_set(&d_ram, &ram_master, 1);                /* RAM-based: dCtlDriver is a handle */
    dce_set(&d_purged, &purged_master, 1);          /* RAM-based, purged */
    dce_set(&d_other, other, 0);
    p_enet = d_enet.b; p_rom = d_rom.b; p_ram = d_ram.b; p_purged = d_purged.b; p_other = d_other.b;
    h_enet = &p_enet; h_rom = &p_rom; h_ram = &p_ram; h_purged = &p_purged; h_other = &p_other;

    T_CHECK(mt_units_find_ipp_in(NULL, 4, all) == NULL);
    tbl[0] = NULL; tbl[1] = h_enet; tbl[2] = h_purged; tbl[3] = h_other;
    T_CHECK(mt_units_find_ipp_in(tbl, 4, all) == NULL);            /* absent */
    tbl[4] = h_ram;
    T_CHECK(mt_units_find_ipp_in(tbl, 5, all) == ram_ipp);         /* RAM handle */
    T_CHECK(mt_units_find_ipp_in(tbl, 4, all) == NULL);            /* n bounds the walk */
    tbl[2] = h_rom;
    T_CHECK(mt_units_find_ipp_in(tbl, 5, all) == rom_ipp);         /* ROM pointer, first match */
    {
        uint8_t *nul = NULL;
        uint8_t **h_nul = &nul;                                      /* handle to a freed DCE */
        tbl[0] = h_nul; tbl[1] = h_enet; tbl[2] = h_purged;
        T_CHECK(mt_units_find_ipp_in(tbl, 3, all) == NULL);
    }
}

static void problem_text_messages(void) {
    CfeFields f;
    CfeError e;
    unsigned char ps[256];
    setf(&f, "", "255.0.0.0", "", "", "");
    T_EQ(cfe_validate(&f, &e), CFE_E_IP);
    cdl_problem_text(&f, CFE_E_IP, &e, ps);
    T_CHECK(pstr_is(ps, "IP address required: the other fields are set."));
    setf(&f, "10.0.0.256", "", "", "", "");
    T_EQ(cfe_validate(&f, &e), CFE_E_IP);
    cdl_problem_text(&f, CFE_E_IP, &e, ps);
    T_CHECK(pstr_has(ps, "IP address is invalid"));
    setf(&f, "10.0.0.2", "255.0.255.0", "", "", "");
    T_EQ(cfe_validate(&f, &e), CFE_E_MASK);
    cdl_problem_text(&f, CFE_E_MASK, &e, ps);
    T_CHECK(pstr_has(ps, "subnet mask"));
    setf(&f, "10.0.0.2", "255.0.0.0", "127.0.0.1", "", "");
    T_EQ(cfe_validate(&f, &e), CFE_E_ADDR_RESERVED);
    cdl_problem_text(&f, CFE_E_ADDR_RESERVED, &e, ps);
    T_CHECK(pstr_is(ps, "The router address is a reserved address."));
    setf(&f, "10.0.0.2", "255.0.0.0", "", "224.0.0.1", "");
    T_EQ(cfe_validate(&f, &e), CFE_E_ADDR_RESERVED);
    cdl_problem_text(&f, CFE_E_ADDR_RESERVED, &e, ps);
    T_CHECK(pstr_is(ps, "The DNS server address is a reserved address."));
    setf(&f, "10.0.0.2", "255.0.0.0", "192.168.1.1", "", "");
    T_EQ(cfe_validate(&f, &e), CFE_E_GW_OFFNET);
    cdl_problem_text(&f, CFE_E_GW_OFFNET, &e, ps);
    T_CHECK(pstr_has(ps, "Note:"));
    cdl_problem_text(&f, CFE_OK, &e, ps);
    T_EQ(ps[0], 0);
    {   /* every code has a sentence that fits */
        int rc;
        for (rc = CFE_E_IP; rc <= CFE_E_GW_SELF; rc++) {
            e.field = CFE_F_DNS; e.code = rc;
            cdl_problem_text(&f, rc, &e, ps);
            T_CHECK(ps[0] > 10);
        }
    }
}

static void check_key_transitions(void) {
    CfeFields f;
    CfeError e;
    int rc, k_required, k_invalid, k_mask;
    setf(&f, "", "255.0.0.0", "", "", "");
    rc = cfe_validate(&f, &e);
    k_required = cdl_check_key(&f, rc, &e);
    setf(&f, "1", "255.0.0.0", "", "", "");          /* typing the first digit: same rc and field */
    rc = cfe_validate(&f, &e);
    T_EQ(rc, CFE_E_IP);
    k_invalid = cdl_check_key(&f, rc, &e);
    T_CHECK(k_required != k_invalid);                  /* the text changes, so must the key */
    setf(&f, "10.0.0.2", "255.0.255.0", "", "", "");
    rc = cfe_validate(&f, &e);
    k_mask = cdl_check_key(&f, rc, &e);
    T_CHECK(k_mask != k_invalid && k_mask != k_required && k_mask != 0);
    setf(&f, "10.0.0.2", "255.0.0.0", "", "", "");
    rc = cfe_validate(&f, &e);
    T_EQ(cdl_check_key(&f, rc, &e), 0);
    T_CHECK(cdl_check_key(&f, rc, &e) != -1);          /* -1 is the "force a redraw" value */
    {   /* reserved router vs reserved DNS: same rc, different field, different text */
        int a, b;
        setf(&f, "10.0.0.2", "255.0.0.0", "127.0.0.1", "", "");
        rc = cfe_validate(&f, &e); a = cdl_check_key(&f, rc, &e);
        setf(&f, "10.0.0.2", "255.0.0.0", "", "127.0.0.1", "");
        rc = cfe_validate(&f, &e); b = cdl_check_key(&f, rc, &e);
        T_CHECK(a != b);
    }
}

static void save_and_close_decisions(void) {
    CfeFields saved, cur, draft;
    CfeError e;
    T_EQ(cdl_save_action(CFE_OK), CDL_SAVE_WRITE);
    T_EQ(cdl_save_action(CFE_E_GW_OFFNET), CDL_SAVE_ASK_WARN);
    T_EQ(cdl_save_action(CFE_E_MASK), CDL_SAVE_REFUSE);
    T_EQ(cdl_save_action(CFE_E_IP), CDL_SAVE_REFUSE);

    setf(&saved, "10.0.0.2", "255.0.0.0", "10.0.0.1", "1.1.1.1", "");
    cur = saved;
    T_EQ(cdl_close_action(&cur, &saved, NULL, cfe_validate(&cur, &e)), CDL_CLOSE_NOTHING);
    setf(&draft, "10.0.0.2", "255.0.255.0", "10.0.0.1", "1.1.1.1", "");
    T_EQ(cdl_close_action(&cur, &saved, &draft, cfe_validate(&cur, &e)), CDL_CLOSE_DROP_DRAFT);
    setf(&cur, "10.0.0.3", "255.0.0.0", "10.0.0.1", "1.1.1.1", "");          /* valid edit */
    T_EQ(cdl_close_action(&cur, &saved, NULL, cfe_validate(&cur, &e)), CDL_CLOSE_ASK);
    T_EQ(cdl_close_action(&cur, &saved, &draft, cfe_validate(&cur, &e)), CDL_CLOSE_ASK);
    setf(&cur, "10.0.0.2", "255.0.0.0", "192.168.1.1", "1.1.1.1", "");       /* warning only */
    T_EQ(cdl_close_action(&cur, &saved, NULL, cfe_validate(&cur, &e)), CDL_CLOSE_ASK);
    cur = draft;                                                             /* invalid edit */
    T_EQ(cdl_close_action(&cur, &saved, NULL, cfe_validate(&cur, &e)), CDL_CLOSE_KEEP_DRAFT);
    T_EQ(cdl_close_action(&cur, &saved, &draft, cfe_validate(&cur, &e)), CDL_CLOSE_NOTHING);
    strcpy(cur.dns, "9.9.9.9");                                              /* edited draft, still invalid */
    T_EQ(cdl_close_action(&cur, &saved, &draft, cfe_validate(&cur, &e)), CDL_CLOSE_KEEP_DRAFT);
    setf(&saved, "", "", "", "", "");                                        /* first run */
    setf(&cur, "", "255.0.0.0", "", "", "");
    T_EQ(cdl_close_action(&cur, &saved, NULL, cfe_validate(&cur, &e)), CDL_CLOSE_KEEP_DRAFT);
}

static void field_caps(void) {
    T_EQ(cdl_field_cap(CFE_F_IP), 15);
    T_EQ(cdl_field_cap(CFE_F_DNS), 15);
    T_EQ(cdl_field_cap(CFE_F_DOMAIN), 63);
    T_EQ(cdl_field_cap(0), 0);
    T_EQ(cdl_insert_ok(14, 0, 1, 15), 1);
    T_EQ(cdl_insert_ok(15, 0, 1, 15), 0);
    T_EQ(cdl_insert_ok(15, 1, 1, 15), 1);              /* typing over a selection */
    T_EQ(cdl_insert_ok(15, 15, 15, 15), 1);
    T_EQ(cdl_insert_ok(10, 0, 6, 15), 0);              /* paste too long */
    T_EQ(cdl_insert_ok(0, 0, 256, 63), 0);
    T_EQ(cdl_key_inserts('5'), 1);
    T_EQ(cdl_key_inserts('.'), 1);
    T_EQ(cdl_key_inserts(8), 0);                       /* backspace */
    T_EQ(cdl_key_inserts(9), 0);                       /* tab */
    T_EQ(cdl_key_inserts(28), 0);                      /* arrows */
    T_EQ(cdl_key_inserts(127), 0);                     /* forward delete */
}

static void draft_round_trip(void) {
    CfeFields f, g;
    uint8_t b[256];
    uint32_t n;
    setf(&f, "10.0.0.2", "255.0.255.0", "10.0.0.1", "", "a-very.long_domain");
    f.log = 1;
    f.dhcp = 1;
    n = cdl_draft_encode(&f, b, sizeof b);
    T_EQ(n, 1 + 1 + 8 + 1 + 11 + 1 + 8 + 1 + 0 + 1 + 18 + 1 + 1);
    T_EQ(b[0], 3);                                     /* Plan 4: version 3, log + dhcp bytes */
    T_EQ(cdl_draft_decode(b, n, &g, 0), 0);
    T_EQ(g.log, 1);                                    /* the flag round-trips, default ignored */
    T_EQ(g.dhcp, 1);
    T_EQ(cdl_fields_equal(&f, &g), 1);
    f.log = 0;
    f.dhcp = 0;
    n = cdl_draft_encode(&f, b, sizeof b);
    T_EQ(cdl_draft_decode(b, n, &g, 1), 0);
    T_EQ(g.log, 0);
    T_EQ(g.dhcp, 0);
    T_EQ(cdl_draft_encode(&f, b, 10), 0);               /* cap too small */
    T_EQ(cdl_draft_encode(&f, b, n - 1), 0);            /* no room for the dhcp byte */
    T_EQ(cdl_draft_decode(b, n - 1, &g, 0), -1);       /* truncated (dhcp byte missing) */
    T_EQ(g.ip[0], 0);
    b[n - 1] = 2;
    T_EQ(cdl_draft_decode(b, n, &g, 0), -1);           /* dhcp byte not 0/1 */
    b[n - 1] = 0;
    b[n - 2] = 2;
    T_EQ(cdl_draft_decode(b, n, &g, 0), -1);           /* log byte not 0/1 */
    b[n - 2] = 0;
    b[0] = 4;
    T_EQ(cdl_draft_decode(b, n, &g, 0), -1);           /* foreign version */
    b[0] = 3; b[1] = 16;                               /* field longer than its buffer */
    T_EQ(cdl_draft_decode(b, n, &g, 0), -1);
    T_EQ(cdl_draft_decode(NULL, 0, &g, 0), -1);
    setf(&f, "", "", "", "", "");
    n = cdl_draft_encode(&f, b, sizeof b);
    T_EQ(n, 8);
    T_EQ(cdl_draft_decode(b, n, &g, 1), 0);
    T_EQ(cdl_fields_equal(&f, &g), 1);
    /* a version 2 draft (Task 11b: log byte, no dhcp byte) decodes with dhcp off */
    {
        static const uint8_t v2[] = { 2, 0, 0, 0, 0, 3,'l','a','n', 1 };
        T_EQ(cdl_draft_decode(v2, sizeof v2, &g, 0), 0);
        T_EQ(g.log, 1);
        T_EQ(g.dhcp, 0);
        T_CHECK(strcmp(g.domain, "lan") == 0);
    }
    /* changing only the DHCP box counts as an edit */
    setf(&f, "", "", "", "", "");
    g = f; g.dhcp = 1;
    T_EQ(cdl_fields_equal(&f, &g), 0);
}

/* Task 8 fix round 1 drafts (version 1, no flag byte) still decode; the flag is the default. */
static void draft_v1_decodes(void) {
    static const uint8_t v1[] = { 1, 8,'1','0','.','0','.','0','.','2', 3,'2','5','5', 0, 0, 3,'l','a','n' };
    CfeFields g;
    T_EQ(cdl_draft_decode(v1, sizeof v1, &g, 1), 0);
    T_CHECK(strcmp(g.ip, "10.0.0.2") == 0);
    T_CHECK(strcmp(g.mask, "255") == 0);
    T_CHECK(strcmp(g.domain, "lan") == 0);
    T_EQ(g.log, 1);
    T_EQ(cdl_draft_decode(v1, sizeof v1, &g, 0), 0);
    T_EQ(g.log, 0);
}

/* Task 11b: a new prefs file defaults the checkbox ON; an existing one shows its flag. */
static void load_saved_defaults(void) {
    static const uint8_t off_file[] = { 0,1, 0,0, 10,0,0,2, 255,0,0,0, 10,0,0,1, 1,1,1,1, 0, 0x00 };
    static const uint8_t on_file[] = { 0,1, 0,0, 10,0,0,2, 255,0,0,0, 10,0,0,1, 1,1,1,1, 0, 0x81 };
    static const uint8_t foreign[] = { 0,9, 0,0 };
    CfeFields f, g;
    cdl_load_saved(NULL, 0, &f);
    T_EQ(f.log, 1);
    T_EQ(f.ip[0], 0);
    cdl_load_saved(foreign, sizeof foreign, &f);
    T_EQ(f.log, 1);
    cdl_load_saved(off_file, sizeof off_file, &f);
    T_EQ(f.log, 0);
    T_CHECK(strcmp(f.ip, "10.0.0.2") == 0);
    cdl_load_saved(on_file, sizeof on_file, &g);
    T_EQ(g.log, 1);
    /* changing only the checkbox counts as an edit */
    g = f; g.log = 1;
    T_EQ(cdl_fields_equal(&f, &g), 0);
    T_EQ(cdl_close_action(&g, &f, NULL, CFE_OK), CDL_CLOSE_ASK);
    T_EQ(cdl_close_action(&f, &f, NULL, CFE_OK), CDL_CLOSE_NOTHING);
}

static void pcat_caps_at_255(void) {
    unsigned char ps[256];
    int i;
    ps[0] = 0;
    for (i = 0; i < 40; i++) cdl_pcat(ps, "0123456789");
    T_EQ(ps[0], 255);
}

static int ptext_is(const unsigned char *ps, const char *c) {
    return ps[0] == strlen(c) && memcmp(ps + 1, c, ps[0]) == 0;
}

/* Plan 4: the status line for every unit / mode / DHCP state. */
static void status_text_lines(void) {
    unsigned char ps[256];
    MtStatusView v;
    memset(&v, 0, sizeof v);
    v.magic = MT_VIEW_MAGIC; v.version = MT_VIEW_VERSION;
    cdl_status_text(CDL_UNIT_NONE, NULL, 0, ps);
    T_CHECK(ptext_is(ps, "Not loaded: restart after saving."));
    cdl_status_text(CDL_UNIT_MACTCP, NULL, 0, ps);
    T_CHECK(ptext_is(ps, "MacTCP is loaded; MoofTCP is off."));
    cdl_status_text(CDL_UNIT_MOOF, NULL, 0, ps);       /* a driver without the view (RC2) */
    T_CHECK(ptext_is(ps, "MoofTCP is loaded."));
    cdl_status_text(CDL_UNIT_MOOF, &v, 0, ps);         /* manual, no address: the carry-over line */
    T_CHECK(ptext_is(ps, "MoofTCP is loaded, not configured."));
    v.addr = 0x0A000002u;
    cdl_status_text(CDL_UNIT_MOOF, &v, 0, ps);
    T_CHECK(ptext_is(ps, "MoofTCP is loaded: 10.0.0.2."));
    v.linked = 1;
    cdl_status_text(CDL_UNIT_MOOF, &v, 0, ps);
    T_CHECK(ptext_is(ps, "MoofTCP is running at 10.0.0.2."));
    memset(&v, 0, sizeof v);
    v.magic = MT_VIEW_MAGIC; v.version = MT_VIEW_VERSION; v.mode = 1;
    cdl_status_text(CDL_UNIT_MOOF, &v, 0, ps);
    T_CHECK(ptext_is(ps, "DHCP: starts with the first network app."));
    v.linked = 1; v.state = MT_DHCP_SELECTING;
    cdl_status_text(CDL_UNIT_MOOF, &v, 0, ps);
    T_CHECK(ptext_is(ps, "DHCP: asking for an address..."));
    v.state = MT_DHCP_BOUND; v.addr = 0xC0A80117u; v.lease_s = 7200; v.t_bound = 1000;
    cdl_status_text(CDL_UNIT_MOOF, &v, 1000 + 60u * 61u, ps);   /* 61 s later */
    T_CHECK(ptext_is(ps, "DHCP: 192.168.1.23 (lease 1h58m)"));
    cdl_status_text(CDL_UNIT_MOOF, &v, 1000 + 60u * 7000u, ps);
    T_CHECK(ptext_is(ps, "DHCP: 192.168.1.23 (lease 3m)"));
    cdl_status_text(CDL_UNIT_MOOF, &v, 1000 + 60u * 8000u, ps);  /* past the end: not yet seen */
    T_CHECK(ptext_is(ps, "DHCP: 192.168.1.23 (lease 0m)"));
    v.lease_s = 5u * 86400u;
    cdl_status_text(CDL_UNIT_MOOF, &v, 1000, ps);
    T_CHECK(ptext_is(ps, "DHCP: 192.168.1.23 (lease 5d)"));
    v.lease_s = 0xFFFFFFFFu;
    cdl_status_text(CDL_UNIT_MOOF, &v, 1000, ps);
    T_CHECK(ptext_is(ps, "DHCP: 192.168.1.23 (no expiry)"));
    v.state = MT_DHCP_RENEWING;
    cdl_status_text(CDL_UNIT_MOOF, &v, 1000, ps);
    T_CHECK(ptext_is(ps, "DHCP: 192.168.1.23, renewing"));
    v.state = MT_DHCP_REBINDING;
    cdl_status_text(CDL_UNIT_MOOF, &v, 1000, ps);
    T_CHECK(ptext_is(ps, "DHCP: 192.168.1.23, renewing"));
    v.state = MT_DHCP_INIT; v.addr = 0;                /* lease lost, asking again */
    cdl_status_text(CDL_UNIT_MOOF, &v, 1000, ps);
    T_CHECK(ptext_is(ps, "DHCP: asking for an address..."));
}

/* Plan 4: the status view through the DCE's dCtlStorage. */
static void units_view_from_dce(void) {
    static uint8_t ipp[64];
    static uint8_t dce[64];
    static MtStatusView v;
    uint8_t *p_dce = dce;
    uint8_t **h = &p_dce;
    uint8_t **tbl[2];
    const uint8_t *found = NULL, *drv;
    void *vp = &v;
    uintptr_t all = ~(uintptr_t)0;
    drv_set(ipp, ".ipp");
    memset(dce, 0, sizeof dce);
    { const void *pp = ipp; memcpy(dce, &pp, sizeof pp); }
    tbl[0] = NULL; tbl[1] = h;
    drv = mt_units_find_ipp_dce_in(tbl, 2, all, &found);
    T_CHECK(drv == ipp);
    T_CHECK(found == dce);
    T_CHECK(mt_units_view_in(found, all) == NULL);      /* storage 0: an older driver */
    memcpy(dce + MT_UNITS_DCE_STORAGE, &vp, sizeof vp);
    T_CHECK(mt_units_view_in(found, all) == NULL);      /* magic missing */
    v.magic = MT_VIEW_MAGIC; v.version = 2;
    T_CHECK(mt_units_view_in(found, all) == NULL);      /* another layout */
    v.version = MT_VIEW_VERSION;
    T_CHECK(mt_units_view_in(found, all) == &v);
    T_CHECK(mt_units_view_in(NULL, all) == NULL);
}

T_MAIN(units_is_moof_matches_moof_r, units_find_ipp_fake_table, problem_text_messages,
       check_key_transitions, save_and_close_decisions, field_caps, draft_round_trip, pcat_caps_at_255,
       draft_v1_decodes, load_saved_defaults, status_text_lines, units_view_from_dce)
