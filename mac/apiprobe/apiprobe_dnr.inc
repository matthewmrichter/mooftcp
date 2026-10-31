/* apiprobe_dnr.inc - ApiProbe group D: the DNR (included by apiprobe.c, uses its helpers).
 *
 * Loads the 'dnrp' resource of "MacTCP DNR" (os_load_dnr) and calls it the way a dnr.c wrapper
 * does: the selector as a long, the arguments after it, C convention. Selector numbers are those of
 * Apple's dnr.c (OpenResolver 1, CloseResolver 2, StrToAddr 3, AddrToStr 4, EnumCache 5,
 * AddrToName 6, HInfo 7, MXInfo 8); the cases show what each does on the stack under test.
 * Line format as the other groups: id name S cs=0 res=<return value> extras | (no events). */
typedef long (*DnrFn)(long sel, ...);
static DnrFn dnr_fn;
typedef struct {
    volatile int hit;
    int32_t rtn;
    char cname[48];
    uint32_t addr[4];
    uint16_t sr;
    uint32_t a5;
    long tick;
} DR;
static DR dr[8];
static hostInfo hinfo[8];
static long dr_t0;

static inline uint32_t get_a5(void) { uint32_t v; __asm__ volatile("move.l %%a5,%0" : "=d"(v)); return v; }
static inline uint16_t get_sr(void) { uint16_t v; __asm__ volatile("move.w %%sr,%0" : "=d"(v)); return v; }

/* The result proc: pascal void (hostInfo *, char *userData). Records what it sees. */
static pascal void dres(hostInfo *hi, char *ud)
{
    int k = (int)(long)ud & 7, i;
    DR *r = &dr[k];
    r->sr = get_sr(); r->a5 = get_a5();
    r->tick = (long)os_ticks() - dr_t0;
    r->rtn = hi->rtnCode;
    for (i = 0; i < 47 && hi->cname[i]; i++) r->cname[i] = hi->cname[i];
    r->cname[i] = 0;
    for (i = 0; i < 4; i++) r->addr[i] = hi->addr[i];
    r->hit++;
}

typedef struct { char *cname; uint16_t type; uint16_t cclass; uint32_t ttl; uint32_t rdata; } CacheRec;
static struct { int n; char name[8][40]; uint16_t type[8], cclass[8]; uint32_t ttl[8], rdata[8]; } en;
static pascal void eres(CacheRec *c, char *ud)
{
    int k = en.n, i;
    (void)ud;
    if (k >= 8) { en.n++; return; }
    if (c->cname) { for (i = 0; i < 39 && c->cname[i]; i++) en.name[k][i] = c->cname[i]; en.name[k][i] = 0; }
    else strcpy(en.name[k], "<null>");
    en.type[k] = c->type; en.cclass[k] = c->cclass; en.ttl[k] = c->ttl; en.rdata[k] = c->rdata;
    en.n++;
}

static void dr_clear(int k)
{
    memset(&dr[k], 0, sizeof dr[k]);
    memset(&hinfo[k], 0, sizeof hinfo[k]);
    hinfo[k].rtnCode = 0x7777;                 /* shows whether the DNR wrote rtnCode */
    dr_t0 = (long)os_ticks();
}
static void dr_wait(int k, long ticks)
{
    long t0 = (long)os_ticks();
    while (!dr[k].hit && (long)os_ticks() - t0 < ticks) { }
}
static void dr_report(int k)
{
    int i;
    kv("hit", dr[k].hit);
    if (dr[k].hit) {
        kt("t", dr[k].tick);
        kv("rtn", dr[k].rtn); ks("cname", dr[k].cname);
        for (i = 0; i < 4; i++)
            if (dr[k].addr[i] || i == 0) { char n[4]; n[0] = 'a'; n[1] = (char)('0' + i); n[2] = 0; kip(n, dr[k].addr[i]); }
        kh("sr", dr[k].sr); kv("a5ok", dr[k].a5 == (uint32_t)get_a5());
    }
}
static void hi_report(int k, const char *tag)
{
    int i;
    ap(" "); ap(tag); ap(".rtn="); apd(hinfo[k].rtnCode);
    ap(" cn=");
    for (i = 0; i < 40 && hinfo[k].cname[i]; i++) { char c[2]; c[0] = hinfo[k].cname[i]; c[1] = 0; ap(c); }
    for (i = 0; i < 4; i++) if (hinfo[k].addr[i]) kip("ad", hinfo[k].addr[i]);
}
static void str_report(const char *s)
{
    int i;
    ap(" s=");
    for (i = 0; i < 20 && s[i]; i++) { char c[2]; c[0] = s[i]; c[1] = 0; ap(c); }
    kv("len", (long)strlen(s));
}

static void dnr_call(const char *id, const char *name, int k, const char *host, int wait)
{
    long r;
    BEGIN(id, name, 0);
    dr_clear(k);
    r = dnr_fn(3L, host, &hinfo[k], dres, (char *)(long)k);
    kt("dt", (long)os_ticks() - dr_t0);
    kv("immediate", r);
    ks("host", host);
    hi_report(k, "hi");
    if (wait) { dr_wait(k, TK(wait)); dr_report(k); hi_report(k, "hiAfter"); }
    emit(0, r);
}

/* HeapInfoDNR's measurement in-process: load the resource, open, one lookup that needs the network,
 * close, dispose; FreeMem of the application and the system zone at each step (bytes taken vs the
 * start). The second cycle is the figure to quote (the first may include a one-time first-use cost). */
static void c_dnr_cycle(const char *id, int n)
{
    unsigned long a0, s0, a1, s1, a2, s2, a3, s3;
    short err; void *code; long r1, r2;
    char host[40];
    BEGIN(id, "heapinfo_cycle", 0);
    a0 = os_app_free(); s0 = os_sys_free();
    code = os_load_dnr(&err);
    if (!code) { kv("err", err); emit(0, err); return; }
    dnr_fn = (DnrFn)code;
    r1 = dnr_fn(1L, (char *)0);
    a1 = os_app_free(); s1 = os_sys_free();
    strcpy(host, "cyc0.172.17.0.7.nip.io"); host[3] = (char)('0' + n);
    dr_clear(1);
    r2 = dnr_fn(3L, host, &hinfo[1], dres, (char *)1L);
    dr_wait(1, TK(15));
    a2 = os_app_free(); s2 = os_sys_free();
    dnr_fn(2L);
    os_free_dnr(code);
    a3 = os_app_free(); s3 = os_sys_free();
    kv("open", r1); kv("lookup", r2); kv("hit", dr[1].hit); kv("rtn", dr[1].rtn);
    kv("appOpen", (long)(a0 - a1)); kv("sysOpen", (long)(s0 - s1));
    kv("appLooked", (long)(a0 - a2)); kv("sysLooked", (long)(s0 - s2));
    kv("appClosed", (long)(a0 - a3)); kv("sysClosed", (long)(s0 - s3));
    emit(0, r1);
}

/* Group Q: the application quits with a lookup pending (Review Focus 1). No alert: the app just exits
 * and probe-run.sh checks that the Finder is still alive after the answer has had time to arrive. */
static void c_dnr_quit(void)
{
    short err; void *code; long r;
    BEGIN("Q00", "dnr_quit_with_pending", 0);
    code = os_load_dnr(&err);
    kv("err", err);
    if (!code) { emit(0, err); return; }
    dnr_fn = (DnrFn)code;
    kv("open", dnr_fn(1L, (char *)0));
    dr_clear(1);
    r = dnr_fn(3L, "quit.172.17.0.9.nip.io", &hinfo[1], dres, (char *)1L);
    kv("immediate", r);
    emit(0, r);
}

static void c_dnr(void)
{
    short err; void *code; long r;
    unsigned long a0, s0, a1, s1;
    char str[40];
    static char ret7[700];

    a0 = os_app_free(); s0 = os_sys_free();
    code = os_load_dnr(&err);
    BEGIN("D00", "dnr_load", 0);
    kv("err", err); kv("codeLoaded", code != 0);
    emit(0, err);
    if (!code) return;
    dnr_fn = (DnrFn)code;

    BEGIN("D01", "open", 0);
    r = dnr_fn(1L, (char *)0);
    a1 = os_app_free(); s1 = os_sys_free();
    kh("appDelta", a0 - a1); kh("sysDelta", s0 - s1);
    emit(0, r);
    BEGIN("D02", "open_again", 0);
    r = dnr_fn(1L, (char *)0);
    emit(0, r);

    dnr_call("D10", "str_ip_literal", 1, "10.1.2.3", 3);
    dnr_call("D11", "str_ip_literal_trailing_dot", 1, "10.1.2.3.", 3);
    dnr_call("D12", "str_nip", 2, "172.17.0.1.nip.io", 15);
    dnr_call("D13", "str_nip_again_cached", 3, "172.17.0.1.nip.io", 3);
    dnr_call("D15", "str_syntax_empty", 5, "", 3);
    dnr_call("D16", "str_syntax_bad_chars", 5, "bad name!.example", 3);
    dnr_call("D17", "str_nip_case", 6, "172.17.0.1.NIP.IO", 8);

    BEGIN("D20", "str_no_resultproc", 0);
    dr_clear(7);
    r = dnr_fn(3L, "172.17.0.2.nip.io", &hinfo[7], (void *)0, (char *)7L);
    kv("immediate", r); hi_report(7, "hi");
    delay(TK(4));
    emit(0, r);
    BEGIN("D21", "str_noproc_now_cached", 0);
    dr_clear(7);
    r = dnr_fn(3L, "172.17.0.2.nip.io", &hinfo[7], (void *)0, (char *)7L);
    kv("immediate", r); hi_report(7, "hi");
    emit(0, r);

    BEGIN("D30", "str_two_concurrent", 0);
    dr_clear(1); dr_clear(2);
    {
        long r1, r2;
        r1 = dnr_fn(3L, "a.172.17.0.3.nip.io", &hinfo[1], dres, (char *)1L);
        r2 = dnr_fn(3L, "b.172.17.0.4.nip.io", &hinfo[2], dres, (char *)2L);
        kv("r1", r1); kv("r2", r2);
        dr_wait(1, TK(10)); dr_wait(2, TK(10));
        dr_report(1); ap(" ##"); dr_report(2);
        emit(0, r1);
    }

    BEGIN("D31", "str_five_concurrent", 0);
    {
        static const char *nm[5] = { "c1.172.17.0.11.nip.io", "c2.172.17.0.12.nip.io", "c3.172.17.0.13.nip.io",
                                     "c4.172.17.0.14.nip.io", "c5.172.17.0.15.nip.io" };
        long rr[5]; int i, got = 0;
        for (i = 0; i < 5; i++) dr_clear(i);
        for (i = 0; i < 5; i++) rr[i] = dnr_fn(3L, nm[i], &hinfo[i], dres, (char *)(long)i);
        for (i = 0; i < 5; i++) kv("r", rr[i]);
        delay(TK(8));
        for (i = 0; i < 5; i++) got += dr[i].hit;
        kv("hits", got);
        emit(0, rr[0]);
    }

    BEGIN("D40", "addr_to_str", 0);
    memset(str, 'Z', sizeof str); str[39] = 0;
    r = dnr_fn(4L, 0xAC110001UL, str);
    str_report(str);
    emit(0, r);
    BEGIN("D41", "addr_to_str_ff00", 0);
    memset(str, 'Z', sizeof str); str[39] = 0;
    r = dnr_fn(4L, 0xFF00FF00UL, str);
    str_report(str);
    emit(0, r);

    BEGIN("D50", "addr_to_name_1111", 0);
    dr_clear(1);
    r = dnr_fn(6L, 0x01010101UL, &hinfo[1], dres, (char *)1L);
    kv("immediate", r); kt("dt", (long)os_ticks() - dr_t0); hi_report(1, "hi");
    dr_wait(1, TK(25)); dr_report(1); hi_report(1, "hiAfter");
    emit(0, r);
    BEGIN("D51", "addr_to_name_1111_again", 0);
    dr_clear(2);
    r = dnr_fn(6L, 0x01010101UL, &hinfo[2], dres, (char *)2L);
    kv("immediate", r); hi_report(2, "hi");
    dr_wait(2, TK(5)); dr_report(2);
    emit(0, r);
    BEGIN("D53", "addr_to_name_two_concurrent", 0);
    dr_clear(4); dr_clear(5);
    {
        long r1, r2;
        r1 = dnr_fn(6L, 0x08080808UL, &hinfo[4], dres, (char *)4L);
        r2 = dnr_fn(6L, 0x09090909UL, &hinfo[5], dres, (char *)5L);
        kv("r1", r1); kv("r2", r2);
        dr_wait(4, TK(25)); dr_wait(5, TK(25));
        dr_report(4); ap(" ##"); dr_report(5);
        emit(0, r1);
    }
    BEGIN("D54", "addr_to_name_no_proc", 0);
    dr_clear(7);
    r = dnr_fn(6L, 0x04040404UL, &hinfo[7], (void *)0, (char *)7L);
    kv("immediate", r); hi_report(7, "hi");
    emit(0, r);

    BEGIN("D55", "addr_to_name_three_concurrent", 0);
    {
        static const unsigned long ad[3] = { 0x08080404UL, 0x01000001UL, 0x09090909UL };
        long rr[3]; int i, got = 0;
        for (i = 0; i < 3; i++) dr_clear(i);
        for (i = 0; i < 3; i++) rr[i] = dnr_fn(6L, ad[i], &hinfo[i], dres, (char *)(long)i);
        for (i = 0; i < 3; i++) kv("r", rr[i]);
        delay(TK(12));
        for (i = 0; i < 3; i++) { got += dr[i].hit; ap(" ["); ap(dr[i].cname); kv("rtn", dr[i].rtn); ap("]"); }
        kv("hits", got);
        emit(0, rr[0]);
    }

    BEGIN("D60", "enum_cache", 0);
    en.n = 0;
    r = dnr_fn(5L, eres, (char *)0);
    kv("n", en.n);
    {
        int i, m = en.n < 8 ? en.n : 8;
        for (i = 0; i < m; i++) {
            ap(" ["); ap(en.name[i]); kv("type", en.type[i]); kv("class", en.cclass[i]);
            kv("ttl", (long)en.ttl[i]); kip("rd", en.rdata[i]); ap("]");
        }
    }
    emit(0, r);

    BEGIN("D70", "hinfo", 0);
    dr_clear(1); memset(ret7, 0, sizeof ret7);
    r = dnr_fn(7L, "172.17.0.1.nip.io", ret7, dres, (char *)1L);
    kv("immediate", r); kv("rtn0", *(int32_t *)ret7);
    dr_wait(1, TK(10)); dr_report(1);
    emit(0, r);
    BEGIN("D71", "mxinfo", 0);
    dr_clear(2); memset(ret7, 0, sizeof ret7);
    r = dnr_fn(8L, "172.17.0.1.nip.io", ret7, dres, (char *)2L);
    kv("immediate", r); kv("rtn0", *(int32_t *)ret7);
    dr_wait(2, TK(10)); dr_report(2);
    emit(0, r);

    dnr_call("D14", "str_dead_name", 4, "nonexistent-host.invalid", 40);
    dnr_call("D18", "str_dotless", 6, "localhost", 20);
    BEGIN("D52", "addr_to_name_172_17_0_1", 0);
    dr_clear(3);
    r = dnr_fn(6L, 0xAC110001UL, &hinfo[3], dres, (char *)3L);
    kv("immediate", r); hi_report(3, "hi");
    dr_wait(3, TK(40)); dr_report(3); hi_report(3, "hiAfter");
    emit(0, r);

    BEGIN("D80", "selector_0", 0);
    r = dnr_fn(0L, 0L, 0L, 0L, 0L); emit(0, r);
    BEGIN("D81", "selector_9", 0);
    r = dnr_fn(9L, 0L, 0L, 0L, 0L); emit(0, r);

    /* close with a lookup pending: does the result proc still fire? */
    BEGIN("D90", "close_with_pending", 0);
    dr_clear(6);
    r = dnr_fn(3L, "pending.172.17.0.5.nip.io", &hinfo[6], dres, (char *)6L);
    kv("immediate", r);
    kv("close", dnr_fn(2L));
    dr_wait(6, TK(10)); dr_report(6);
    emit(0, r);
    BEGIN("D91", "reopen_after_close", 0);
    r = dnr_fn(1L, (char *)0);
    dr_clear(7);
    kv("lookup", dnr_fn(3L, "10.9.9.9", &hinfo[7], dres, (char *)7L));
    hi_report(7, "hi");
    emit(0, r);
    /* closing twice crashes real MacTCP's DNR (bomb after D91, measured): not probed */
    BEGIN("D92", "close_final", 0);
    r = dnr_fn(2L);
    emit(0, r);
    os_free_dnr(code);
    BEGIN("D99", "dispose", 0);
    kh("appFreeNow", os_app_free()); kh("appFreeStart", a0);
    kh("sysFreeNow", os_sys_free()); kh("sysFreeStart", s0);
    emit(0, 0);
    c_dnr_cycle("D95", 1);
    c_dnr_cycle("D96", 2);
}
