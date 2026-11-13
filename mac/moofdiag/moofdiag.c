/* moofdiag.c - MoofDiag: what the MoofTCP driver looks like from an application (Task 10).
 *
 * Writes MoofDiag.out (CR separated) beside the app:
 *   - the unit table: size, every installed driver with its unit, refnum, name and DCE flags;
 *   - OpenDriver(".IPP"): result and refnum;
 *   - if the driver is open, its resident block through the private Status call 'MT'
 *     (mt_drv.h MT_DRV_STATUS_RES): boot results, configuration, entry counters, gate, Ethernet
 *     and core counters, and the system-heap figures (FreeMem / zone size).
 * Last line: "RESULT PASS" when .IPP opened with refnum -49 and the block was readable, else
 * "RESULT FAIL" (or "RESULT UNCONFIGURED" when the open failed with -23004). */
#include <Quickdraw.h>
#include <Dialogs.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Memory.h>
#include <Resources.h>
#include <Files.h>
#include <Devices.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include "mt.h"
#include "mt_drv.h"
#include "diag_streams.h"
#include "runid.h"
#include "mt_tune_abi.h"
#ifdef MT_TRACE
#include "mt_trace_abi.h"
#endif
#pragma GCC diagnostic ignored "-Warray-bounds"   /* before mt_units.h: its low-memory reads are inlined into main */
#include "mt_units.h"
#include <stddef.h>

QDGlobals qd;

#define UTABLE_BASE (*(uint8_t ***volatile *)0x11C)
#define UNIT_COUNT  (*(volatile int16_t *)0x1D2)
#define SYS_ZONE    (*(THz *)0x2A6)

/* 1 if a run file (mac/common/runid.h) exists and starts with '!'. Only then is runid_name called: it would
 * otherwise ask for an id in a dialog (DLOG 200), which MoofDiag has no resource for and which the unattended
 * Snow runners (no run file) could not answer. */
static int has_run_file(void)
{
    static const char *const names[2] = { RUNID_FILE, RUNID_FILE_OLD };
    int k;
    for (k = 0; k < 2; k++) {
        short r;
        long len = 1;
        char c = 0;
        if (FSOpen((ConstStr255Param)names[k], 0, &r) != noErr) continue;
        if (FSRead(r, &len, &c) != noErr && len <= 0) c = 0;
        FSClose(r);
        return c == '!';
    }
    return 0;
}

/* The MoofTCP release version for the stack: line: the short string of 'vers' 1 in the "MoofTCP" file of the
 * System Folder (the resident block carries none). "?" when the file or the resource cannot be read. The
 * resource fork is opened read-only and closed again only if it was not open already (found by walking the
 * FCB table: a second OpenResFile of an open fork returns the same refnum). */
static void release_vers(char *out, size_t cap)
{
    static const unsigned char fname[] = "\pMoofTCP";
    HParamBlockRec vp;
    FCBPBRec fp;
    Str255 nm, path;
    short vref, res;
    long dir;
    int was_open = 0, idx;
    Handle h;
    short save = CurResFile();

    strcpy(out, "?");
    memset(&vp, 0, sizeof vp);
    path[0] = 0;
    vp.volumeParam.ioNamePtr = path;                         /* the boot volume's name */
    vp.volumeParam.ioVRefNum = *(volatile short *)0x210;     /* BootDrive */
    if (PBHGetVInfoSync(&vp) != noErr) return;
    vref = vp.volumeParam.ioVRefNum;
    dir = vp.volumeParam.ioVFndrInfo[0];                     /* the System Folder */
    if (dir == 0) return;
    for (idx = 1; idx < 64; idx++) {
        memset(&fp, 0, sizeof fp);
        nm[0] = 0;
        fp.ioNamePtr = nm;
        fp.ioVRefNum = vref;
        fp.ioFCBIndx = idx;
        if (PBGetFCBInfoSync(&fp) != noErr) break;
        if (fp.ioFCBParID == dir && (fp.ioFCBFlags & 0x0200) && nm[0] == fname[0] && memcmp(nm + 1, fname + 1, nm[0]) == 0)
            was_open = 1;
    }
    if (path[0] == 0 || path[0] > 27 + 1) return;
    memcpy(path + 1 + path[0], ":System Folder:MoofTCP", 22);   /* OpenResFile: no OpenRFPerm trap before System 7 */
    path[0] = (unsigned char)(path[0] + 22);
    res = OpenResFile(path);
    if (res < 0) return;
    h = Get1Resource('vers', 1);
    if (h && GetHandleSize(h) > 8) {
        unsigned char *d = (unsigned char *)*h;
        unsigned n = d[6];                                   /* 4 bytes number, region word, short string */
        if (n > 0 && n < cap && 7 + n <= (unsigned)GetHandleSize(h)) {
            memcpy(out, d + 7, n);
            out[n] = 0;
        }
    }
    if (!was_open) CloseResFile(res);
    UseResFile(save);
}

static short outref = -1;
static void P(const char *fmt, ...)
{
    char b[256];
    va_list ap;
    long n;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    n = (long)strlen(b);
    if (outref >= 0) FSWrite(outref, &n, b);
}

static void Pline(const char *s) { P("%s", s); }

static uint32_t s24(const void *p) { return (uint32_t)(uintptr_t)p & 0x00FFFFFFu; }
static void ip(char *b, uint32_t a)
{
    sprintf(b, "%lu.%lu.%lu.%lu", (unsigned long)(a >> 24), (unsigned long)(a >> 16 & 255),
            (unsigned long)(a >> 8 & 255), (unsigned long)(a & 255));
}

#ifdef MT_TRACE
/* Perf builds with the echo-path trace (task 18): freeze the driver's ring, write every kept record as one line
 *   TR <t_us> <event> <a hex> <b> <c hex> <d hex>
 * (fields: mt_trace_abi.h), oldest first, then clear it (recording resumes). scripts/trace-echo.py reads them. The
 * header line gives the counts and the clock's read cost; a driver without the trace answers the Control with -4. */
static int16_t trace_call(short ref, MtTraceBlk *b, uint16_t op)
{
    CntrlParam cp;
    void *bp = b;
    memset(&cp, 0, sizeof cp);
    b->version = MT_TRACE_ABI_VERSION;
    b->op = op;
    cp.ioCRefNum = ref;
    cp.csCode = MT_CS_TRACE;
    memcpy(&cp.csParam[0], &bp, 4);
    return PBControlSync((ParmBlkPtr)&cp);
}

static void trace_dump(short ref)
{
    static const char *const nm[MT_TRACE_NEV] = { "?", "RX", "RXRUN", "DATA", "PBIN", "PBOUT", "PBDONE", "EWRITE",
                                                    "EWDONE", "TICK", "TDEFER" };
    static MtTraceRec buf[64];
    MtTraceBlk b;
    uint16_t kept, i;
    int16_t e;
    memset(&b, 0, sizeof b);
    e = trace_call(ref, &b, MT_TRACE_OP_FREEZE);
    if (e != 0) { P("trace: none (Control 0x4D56 = %d)\r", e); return; }
    kept = b.got;
    P("trace: version %u kept %u total %lu cap %u rec %u lap_us %lu now_us %lu clock %s\r", MT_TRACE_ABI_VERSION,
      kept, (unsigned long)b.total, b.cap, b.rec_bytes, (unsigned long)b.lap_us, (unsigned long)b.now_us,
      (b.flags & MT_TRACE_F_TMCLK) ? "timemgr" : "none");
    for (b.first = 0; b.first < kept; b.first = (uint16_t)(b.first + b.got)) {
        b.max = 64;
        b.buf = (uint32_t)(uintptr_t)buf;
        if (trace_call(ref, &b, MT_TRACE_OP_READ) != 0 || b.got == 0) break;
        for (i = 0; i < b.got; i++)
            P("TR %lu %s %02x %u %08lx %08lx\r", (unsigned long)buf[i].t, nm[buf[i].ev < MT_TRACE_NEV ? buf[i].ev : 0],
              buf[i].a, buf[i].b, (unsigned long)buf[i].c, (unsigned long)buf[i].d);
    }
    e = trace_call(ref, &b, MT_TRACE_OP_CLEAR);
    P("trace: %u records written, ring cleared (%d)\r", (unsigned)b.first, e);
}
#endif

static void units(void)
{
    int16_t i, n = UNIT_COUNT;
    P("unit table: %d entries at %08lx\r", n, (unsigned long)s24(UTABLE_BASE));
    for (i = 0; i < n; i++) {
        uint8_t **h = UTABLE_BASE[i];
        const uint8_t *dce, *drv;
        uint16_t fl;
        char name[40];
        if (!h || !*h) continue;
        dce = (const uint8_t *)s24(*h);
        fl = (uint16_t)(dce[4] << 8 | dce[5]);
        drv = (const uint8_t *)s24(*(void *const *)dce);
        if ((fl & 0x40) && drv) drv = *(void *const *)drv ? (const uint8_t *)s24(*(void *const *)drv) : NULL;
        name[0] = 0;
        if (drv) {
            uint8_t k = drv[18] < 39 ? drv[18] : 39;
            memcpy(name, drv + 19, k);
            name[k] = 0;
        }
        P("  unit %2d refnum %4d flags %04x driver %08lx %s\r", i, -(i + 1), fl,
          (unsigned long)s24(drv), name);
    }
}

int main(void)
{
    char line[160], a1[20], a2[20], a3[20], a4[20];
    Str255 ps;
    unsigned char oname[40];
    size_t n;
    short ref = 0;
    OSErr r;
    int pass = 0, unconf = 0, opened = 0, tune_ok = 0;
    THz save;
    long sysfree, syszone;

    MaxApplZone();
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(0);
    InitCursor();
    /* "MoofDiag-<id>.out" when the run file ("Agent Run", mac/common/runid.c) is "!<id>" or "!!<id>", else "MoofDiag.out" */
    if (has_run_file()) runid_name("MoofDiag", ".out", oname);
    else memcpy(oname, "\x0cMoofDiag.out", 13);
    Create((ConstStringPtr)oname, 0, 'ttxt', 'TEXT');
    if (FSOpen((ConstStringPtr)oname, 0, &outref)) outref = -1;
    else SetEOF(outref, 0);                /* each run replaces the file: no stale tail from a longer earlier run */
    P("MoofDiag at tick %lu\r", (unsigned long)TickCount());

    save = GetZone();
    SetZone(SYS_ZONE);
    sysfree = FreeMem();
    syszone = (long)((char *)SYS_ZONE->bkLim - (char *)SYS_ZONE);
    SetZone(save);
    P("system zone: size %ld free %ld occupied %ld\r", syszone, sysfree, syszone - sysfree);

    units();
    r = OpenDriver((ConstStringPtr)"\p.IPP", &ref);
    P("OpenDriver(.IPP) = %d refnum %d\r", r, ref);
    if (r != 0 && r != -23004) P("stack: none (OpenDriver %d)\r", r);
    save = GetZone();
    SetZone(SYS_ZONE);
    sysfree = FreeMem();
    SetZone(save);
    P("system zone after open: size %ld free %ld occupied %ld (no-network baseline: size 162100, occupied 149248)\r",
      syszone, sysfree, syszone - sysfree);
    if (r == -23004) unconf = 1;
    opened = r == 0;
    if (r == 0 || unconf) {
        CntrlParam pb;
        MtRes *m = NULL;
        memset(&pb, 0, sizeof pb);
        const uint8_t *dce0 = NULL, *drv0 = r == 0 ? mt_units_find_ipp_dce(&dce0) : NULL;
        if (r == 0 && drv0 && !mt_units_is_moof(drv0)) {
            /* MacTCP's own driver is open: never send it our private Status code (the unit table says whose driver it
             * is, as in the control panel). Under real MacTCP in Snow that call never returned and hung the run. */
            P("stack: MacTCP (no MoofTCP status)\r");           /* the harness reads this line to learn which stack is live */
            P("Status('MT') not sent: the unit's driver is not MoofTCP\r");
            r = -1;
        } else if (r == 0) {
            pb.ioCRefNum = ref;
            pb.csCode = MT_DRV_STATUS_RES;
            r = PBStatusSync((ParmBlkPtr)&pb);
            memcpy(&m, &pb.csParam[0], 4);
            if (r != 0) P("stack: MacTCP (no MoofTCP status)\r");   /* the harness reads this line to learn which stack is live */
            P("Status('MT') = %d block %08lx version %u\r", r, (unsigned long)s24(m), (unsigned)pb.csParam[2]);
        } else {
            /* Plan 4: the open failed (no address yet, e.g. DHCP without a server): Status needs an open
             * driver, so reach the block through the status view in the unit's dCtlStorage (mt_view.h) */
            const uint8_t *dce = NULL, *drv = mt_units_find_ipp_dce(&dce);
            const MtStatusView *v = drv && mt_units_is_moof(drv) ? mt_units_view_in(dce, MT_UNITS_MASK24) : NULL;
            if (v) m = (MtRes *)(void *)((const char *)v - offsetof(MtRes, view));
            pb.csParam[2] = MT_STATUS_VERSION;
            r = m ? 0 : -1;
            if (!m) P("stack: MoofTCP unconfigured (status view not found)\r");
            P("open failed: block %08lx from the unit's status view\r", (unsigned long)s24(m));
        }
        if (r == 0 && m && pb.csParam[2] != MT_STATUS_VERSION)
            P("stack: MoofTCP status%u unknown\r", (unsigned)pb.csParam[2]);
        else if (r == 0 && m) {
            char rv[40];
            release_vers(rv, sizeof rv);
            P("stack: MoofTCP %s %s\r", rv, m->variant == MT_RES_VARIANT_LITE ? "lite" : "full");
            if (opened) {
                /* a perf build (MT_TUNE) answers the private tune Control (mt_tune_abi.h) with its levers; a release
                 * driver refuses it (-4), and then there is no tune: line. Only MoofTCP's own driver gets here. */
                static uint16_t tb[MT_TW_N];
                CntrlParam tp;
                void *tbp = tb;
                memset(&tp, 0, sizeof tp);
                tb[MT_TW_VERSION] = MT_TUNE_ABI_VERSION;
                tb[MT_TW_OP] = MT_TUNE_OP_GET;
                tp.ioCRefNum = ref;
                tp.csCode = MT_CS_TUNE;
                memcpy(&tp.csParam[0], &tbp, 4);
                if (PBControlSync((ParmBlkPtr)&tp) == 0) {
                    tune_ok = 1;
                    P("tune: rto_min=%u rto_init=%u delack=%u stall=%u quickack=%u txpump=%u\r", (unsigned)tb[MT_TW_RTO_MIN],
                      (unsigned)tb[MT_TW_RTO_INIT], (unsigned)tb[MT_TW_DELACK], (unsigned)tb[MT_TW_STALL],
                      (unsigned)tb[MT_TW_QUICKACK], (unsigned)tb[MT_TW_TXPUMP]);
                }
            }
        }
        if (r == 0 && m && (pb.csParam[2] != MT_STATUS_VERSION || m->version != MT_STATUS_VERSION)) {
            /* another layout: do not read a single field of it */
            P("VERSION MISMATCH: the driver's Status layout is version %u (block says %u), this MoofDiag reads version %d. Rebuild MoofDiag and the driver from the same sources.\r",
              (unsigned)pb.csParam[2], (unsigned)m->version, MT_STATUS_VERSION);
        } else if (r == 0 && m && m->variant != MT_RES_VARIANT) {
            /* the core's mt_stack differs between the full build and MoofTCP Lite: read nothing of it */
            P("VARIANT MISMATCH: the driver is the %s build, this MoofDiag reads the %s build. Use the MoofDiag built with the driver (build-mac/ full, build-mac-lite/ Lite).\r",
              m->variant == MT_RES_VARIANT_LITE ? "Lite" : "full", MT_RES_VARIANT == MT_RES_VARIANT_LITE ? "Lite" : "full");
        } else if (r == 0 && m) {
            const mt_stats *st;
            mt_stack *s;
            P("boot: refnum %d cfg_result %d enet_err %d mode %u size %lu raw %08lx variant %s\r", m->refnum,
              m->cfg_result, m->enet_err, m->mode, (unsigned long)m->size, (unsigned long)s24(m->raw),
              m->variant == MT_RES_VARIANT_LITE ? "Lite" : "full");
            ip(a1, m->cfg.addr); ip(a2, m->cfg.mask); ip(a3, m->cfg.gateway); ip(a4, m->cfg.dns);
            P("config: addr %s mask %s gateway %s dns %s domain '%s' mtu %u\r", a1, a2, a3, a4,
              m->cfg.domain, m->cfg.mtu);
            P("view: magic %04x version %u mode %u state %u linked %u addr %08lx lease %lu t_bound %lu\r",
              m->view.magic, m->view.version, m->view.mode, m->view.state, m->view.linked,
              (unsigned long)m->view.addr, (unsigned long)m->view.lease_s, (unsigned long)m->view.t_bound);
            {
                mt_stack *ds;
                memcpy(&ds, m->d, sizeof ds);        /* MtDrv's first member: mt_stack *s */
#ifdef MT_NO_DHCP
                P("dhcp: not in this build (MoofTCP Lite); prefs mode word %u runs as manual\r", m->mode);
#else
                {
                mt_dhcp_info di;
                mt_dhcp_get_info(ds, &di, mt_ms_from_ticks((uint32_t)TickCount()));
                ip(a1, di.addr); ip(a2, di.server); ip(a3, m->dhcp_hint);
                P("dhcp: state %u addr %s server %s hint %s lease %lu left %lu tx %lu offers %lu acks %lu naks %lu bad %lu\r",
                  di.state, a1, a2, a3, (unsigned long)di.lease_s, (unsigned long)di.left_s,
                  (unsigned long)di.tx, (unsigned long)di.offers, (unsigned long)di.acks,
                  (unsigned long)di.naks, (unsigned long)di.bad);
                }
#endif
                {
                    mt_ifconfig cc;
                    mt_get_config(ds, &cc);          /* what the core runs with (DHCP: the lease) */
                    ip(a1, cc.addr); ip(a2, cc.mask); ip(a3, cc.gateway); ip(a4, cc.dns);
                    P("core config: addr %s mask %s gateway %s dns %s domain '%s'\r", a1, a2, a3, a4, cc.domain);
                }
            }
            P("entries: calls %lu refused %lu pending-run %lu queued %lu queue-full %lu goodbye %lu\r",
              (unsigned long)m->entry.n_calls, (unsigned long)m->entry.n_refused,
              (unsigned long)m->entry.n_pend_run, (unsigned long)m->entry.pend.queued,
              (unsigned long)m->entry.pend.full, (unsigned long)m->entry.n_goodbye);
            P("entries: adopted %lu goodbye-forced %lu refusal-ioResult-only %lu dropped-at-goodbye %lu\r",
              (unsigned long)m->entry.n_adopted, (unsigned long)m->entry.n_goodbye_forced,
              (unsigned long)m->entry.n_compl_skipped, (unsigned long)m->entry.n_pend_dropped);
            P("gate: rx %lu deferred %lu ticks %lu (deferred %lu) after %lu drops %u max ring %lu max depth %lu\r",
              (unsigned long)m->gate.n_rx, (unsigned long)m->gate.n_deferred,
              (unsigned long)m->gate.n_tick, (unsigned long)m->gate.n_tick_deferred,
              (unsigned long)m->gate.n_after, m->gate.drops, (unsigned long)m->gate.max_ring,
              (unsigned long)m->gate.max_depth);
            P("enet: refnum %d mac %02x:%02x:%02x:%02x:%02x:%02x tx ok %lu pad %lu busy %lu len %lu closed %lu err %lu last_err %d\r",
              m->enet.refnum, m->enet.mac[0], m->enet.mac[1], m->enet.mac[2], m->enet.mac[3],
              m->enet.mac[4], m->enet.mac[5], (unsigned long)m->enet.tx_ok,
              (unsigned long)m->enet.tx_pad, (unsigned long)m->enet.tx_drop_busy,
              (unsigned long)m->enet.tx_drop_len, (unsigned long)m->enet.tx_drop_closed,
              (unsigned long)m->enet.tx_err, m->enet.last_err);
            P("enet wd: tx_stuck %lu tx_killed %lu reattach %lu fail %lu quarantined %d blocked %lu abandoned %lu pending %u tries %u opened %u\r",
              (unsigned long)m->enet.tx_stuck, (unsigned long)m->enet.tx_killed,
              (unsigned long)m->enet.reattach, (unsigned long)m->enet.reattach_fail,
              mt_enet_tx_quarantined(&m->enet), (unsigned long)m->enet.reattach_blocked,
              (unsigned long)m->enet.reattach_abandoned, (unsigned)m->enet.reattach_req,
              (unsigned)m->enet.reattach_tries, (unsigned)m->enet.opened);
            {   /* 1.1d2: enough to tell a stopped timer on hardware: a shot armed but due well in the past is lost
                 * (mt_timer_keep re-arms it at the next gate session); the cached deadlines live in the core and the
                 * glue and are not read from here (a query would rebuild them outside the gate) */
                uint32_t now = mt_ms_from_ticks((uint32_t)TickCount());
                P("timer: fired %lu armed %u in_cb %u inserted %u due %lu now %lu due-now %ld early-armed %u\r",
                  (unsigned long)m->timer.fired, m->timer.armed, m->timer.in_cb, m->timer.inserted,
                  (unsigned long)m->tm_due, (unsigned long)now, (long)(int32_t)(m->tm_due - now), m->armed);
            }
#ifndef MT_TUNE
            (void)tune_ok;
#else
            if (tune_ok)   /* a release driver's MtRes ends before .pump: read it only from a driver that answered the tune call */
                P("txpump: on %u runs %lu issued %lu nested %lu max queue %u queued %u task fired %lu hold %u (with the pump, enet tx ok counts frames when queued)\r",
                  (unsigned)m->pump.on, (unsigned long)m->pump.runs, (unsigned long)m->pump.issued,
                  (unsigned long)m->pump.nested, (unsigned)m->pump.max_q,
                  (unsigned)((m->pump.head - m->pump.tail) & (MT_ENET_PUMP_Q - 1u)), (unsigned long)m->pump.tm.fired,
                  (unsigned)m->pump.hold);
#endif
#ifdef MT_TRACE
            if (opened) trace_dump(ref);         /* perf builds with the trace: the TR lines (task 18) */
#endif
            if (!(m->stk.flags & 1))
                P("stack: not built with marks (rebuild with MT_EXTRA_CFLAGS=-DMT_STACK_MARK=1 -DMT_STACK_PAINT=4096)\r");
            else
            P("stack: marks %u paint %u low %08lx entry %08lx used %lu painted %lu episodes %lu (interrupt-time depth below the outermost entry; mt_stackmark.h)\r",
              (unsigned)(m->stk.flags & 1), (unsigned)(m->stk.flags >> 1 & 1), (unsigned long)s24((const void *)m->stk.low), (unsigned long)s24((const void *)m->stk.entry),
              (unsigned long)m->stk.used, (unsigned long)m->stk.deep, (unsigned long)m->stk.n_enter);
            memcpy(&s, m->d, sizeof s);                  /* MtDrv's first member: mt_stack *s */
            st = mt_get_stats(s);
            P("core: eth_in %lu eth_out %lu tx_err %lu arp_in %lu arp_out %lu ip_in %lu ip_out %lu tcp_in %lu tcp_out %lu rexmt %lu udp_in %lu udp_out %lu\r",
              (unsigned long)st->eth_in, (unsigned long)st->eth_out, (unsigned long)st->eth_tx_err,
              (unsigned long)st->arp_in, (unsigned long)st->arp_out, (unsigned long)st->ip_in,
              (unsigned long)st->ip_out, (unsigned long)st->tcp_in, (unsigned long)st->tcp_out,
              (unsigned long)st->tcp_rexmt, (unsigned long)st->udp_in, (unsigned long)st->udp_out);
            P("core drops: arp %lu ip_in %lu reasm_fail %lu udp %lu noport %lu tcp_in %lu fast_rexmt %lu rst_in %lu rst_out %lu; conns_open %lu active_opens %lu passive_opens %lu tx_stalls %lu lo_out %lu lo_drops %lu\r",
              (unsigned long)st->arp_drops, (unsigned long)st->ip_in_drops, (unsigned long)st->ip_reasm_fail,
              (unsigned long)st->udp_drops, (unsigned long)st->udp_noport, (unsigned long)st->tcp_in_drops,
              (unsigned long)st->tcp_fast_rexmt, (unsigned long)st->tcp_resets_in, (unsigned long)st->tcp_resets_out,
              (unsigned long)st->tcp_conns_open, (unsigned long)st->tcp_active_opens, (unsigned long)st->tcp_passive_opens,
              (unsigned long)st->tcp_tx_stalls, (unsigned long)st->ip_lo_out, (unsigned long)st->ip_lo_drops);
            diag_streams(m->d, Pline);               /* streams and MultiFinder owners (Task 12) */
            P("log: prefs flags %02x (bit 0 = diagnostic log on)\r", m->cfg_flags);
            {
                mt_logrec rec[16];
                uint16_t got, tot = 0;
                static const char *const nm[] = { "?", "TCP_STATE", "TCP_TERM", "TCP_REXMT", "DROP", "ARP_FAIL", "DNS_FAIL", "DHCP" };
                /* mt_log_read at task time = the one reader context; the driver writes at gate time */
                while ((got = mt_log_read(s, rec, 16)) != 0) {
                    uint16_t i;
                    for (i = 0; i < got; i++)
                        P("log: t %lu %s a %u b %u\r", (unsigned long)rec[i].t,
                          nm[rec[i].code < 8 ? rec[i].code : 0], rec[i].a, rec[i].b);
                    tot = (uint16_t)(tot + got);
                    if (tot >= 1000) break;
                }
                P("log: %u records\r", tot);
            }
            P("sizes: MtRes %lu MtGate %lu MtEnet %lu MtTimer %lu\r", (unsigned long)sizeof(MtRes),
              (unsigned long)sizeof(MtGate), (unsigned long)sizeof(MtEnet), (unsigned long)sizeof(MtTimer));
            pass = ref < 0;   /* any real driver refnum: the unit is the first free one (-49 in Snow, -50 on a Mac whose .DSP holds unit 48) */
        }
    }
    P("RESULT %s\r", pass ? "PASS" : unconf ? "UNCONFIGURED" : "FAIL");
    if (outref >= 0) { FSClose(outref); outref = -1; }
    FlushVol(0, 0);
    if (runid_unattended()) return 0;      /* "!!" run file: nothing waits for a click */
    sprintf(line, "MoofDiag %s. Results are in %.*s", pass ? "finished" : "FAIL", (int)oname[0], (char *)oname + 1);
    n = strlen(line);
    ps[0] = (unsigned char)n;
    memcpy(ps + 1, line, n);
    ParamText(ps, (ConstStringPtr)"\p", (ConstStringPtr)"\p", (ConstStringPtr)"\p");
    Alert(128, 0);
    return 0;
}
