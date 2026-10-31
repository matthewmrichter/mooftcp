/* mt_dnr.c - `dnrp`, MoofTCP's resolver code resource (Task 11).
 *
 * An application's dnr.c wrapper loads this resource, locks it and calls its start (mt_dnr_start.s)
 * as a C function: the selector as a long, the arguments after it (docs/dev/MACTCP-INTERFACE.md "DNR
 * selectors"). Everything the real DNR does is app-visible only through these calls, the result
 * proc and the hostInfo / returnRec it fills; the work is the driver's: this code sends the private
 * Control call MT_CS_DNR (mt_dnr_abi.h) to ".IPP" and the driver resolves with the core's DNS client
 * and writes the answer into the caller's record while the PB is pending. This code holds no
 * result memory: the state below is the open flag, the driver refnum and four PBs.
 *
 * Context: OpenResolver / StrToAddr / ... run at task time in the application. A lookup that needs
 * the network returns cacheFault (-23042) and the driver completes our PB later, at interrupt time,
 * with the application's A5 (the driver's completion convention): `done` then calls the result proc
 * (pascal void (*)(hostInfo *, char *userData)) from there, as the real DNR does (matrix D12:
 * the proc runs at interrupt level 1 with the app's A5). Nothing here allocates memory, and no
 * global lives in the bss (the section attribute keeps it in the resource image, so relocation has
 * no allocation to make).
 *
 * Only Toolbox headers are included next to mt_dnr_abi.h; mt_mactcp.h would clash with them. */
#include <Devices.h>
#include <Retro68Runtime.h>
#include <stdint.h>
#include "mt_dnr_abi.h"

/* Result codes (obs, matrix "DNR"). */
#define E_SYNTAX      (-23041)
#define E_CACHEFAULT  (-23042)
#define E_DNRERR      (-23047)
#define E_OUTOFMEM    (-23048)

#pragma GCC diagnostic ignored "-Warray-bounds"   /* low-memory global */
#define NQ 4                         /* lookups in flight (the real DNR allows 3) */
#define LM_TIME (*(volatile uint32_t *)0x20C)   /* Time: seconds since 1904 */

/* Selectors. */
enum { S_OPEN = 1, S_CLOSE, S_STRTOADDR, S_ADDRTOSTR, S_ENUMCACHE, S_ADDRTONAME, S_HINFO, S_MXINFO };

/* One lookup. The PB is first: the completion routine receives it and it is the record. */
typedef struct {
    MtDnrPb  pb;
    uint32_t proc, ud;                /* the caller's result proc and userDataPtr */
    int16_t  active;                  /* the result proc is still owed */
    int16_t  late;                    /* the driver queued the PB: its completion (deferred or not) is the result */
} Q;

/* cacheEntryRecord as EnumCache hands it out (obs, matrix D60). */
#pragma pack(push, 2)
typedef struct { uint32_t cname; uint16_t type, cclass; uint32_t ttl; uint32_t rdata; } CacheRec;
#pragma pack(pop)

static struct {
    int16_t   drv;                    /* .IPP refnum */
    int16_t   open;
    int16_t   freed;                  /* Retro68FreeGlobals ran (at most once per load) */
    Q         q[NQ];
    MtDnrPb   cpb;                    /* synchronous calls: cancel, enum */
    MtDnrEnum en;
} g __attribute__((section(".data")));

extern void mt_dnr_call2(uint32_t proc, uint32_t a, uint32_t b);

static void zero(void *p, unsigned n) {
    uint8_t *b = (uint8_t *)p;
    while (n--) *b++ = 0;
}

static void put_rtn(uint32_t rec, int32_t v) {   /* hostInfo.rtnCode, byte-wise: the record may sit anywhere */
    uint8_t *o = (uint8_t *)(uintptr_t)rec;
    o[0] = (uint8_t)((uint32_t)v >> 24); o[1] = (uint8_t)((uint32_t)v >> 16);
    o[2] = (uint8_t)((uint32_t)v >> 8);  o[3] = (uint8_t)v;
}

/* ---- the completion routine (A0 = PB, the same pointer at 4(SP); plain RTS) ---- */

static void done(MtDnrPb *pb) {
    Q *q = (Q *)pb;
    uint32_t proc, ud, out;
    if (!q->active || !(pb->deferred || q->late)) return;   /* finished inside the call, or cancelled */
    q->active = 0;                               /* free before the proc runs: it may look up again */
    proc = q->proc; ud = q->ud; out = pb->out;
    if (pb->ioResult != 0) put_rtn(out, pb->ioResult);      /* a late refusal (the driver queued it) */
    if (proc) mt_dnr_call2(proc, out, ud);
}

/* ---- a lookup ---- */

static long lookup(int op, const char *name, uint32_t addr, uint32_t out, uint32_t proc, uint32_t ud) {
    Q *q = 0;
    int i;
    long err;
    if (!g.open) return E_DNRERR;
    for (i = 0; i < NQ; i++)
        if (!g.q[i].active) { q = &g.q[i]; break; }
    if (!q) { put_rtn(out, E_OUTOFMEM); return E_OUTOFMEM; }
    zero(&q->pb, sizeof q->pb);
    q->pb.ioCompletion = (uint32_t)(uintptr_t)done;
    q->pb.ioCRefNum = g.drv;
    q->pb.csCode = MT_CS_DNR;
    q->pb.op = (int16_t)op;
    q->pb.name = (uint32_t)(uintptr_t)name;
    q->pb.arg = addr;
    q->pb.out = out;
    q->pb.hasproc = proc != 0;
    q->proc = proc;
    q->ud = ud;
    q->late = 0;
    q->active = 1;                              /* before the call: the completion may come at once */
    q->pb.ioResult = 1;
    err = PBControlAsync((ParmBlkPtr)&q->pb);
    if (q->pb.deferred) return E_CACHEFAULT;     /* the driver owns the PB; the result proc follows */
    if (err && q->pb.ioResult == 1) { q->active = 0; return err; }   /* the Device Manager refused the call */
    if (q->pb.ioResult == 1) { q->late = 1; return E_CACHEFAULT; }   /* the driver queued it (an interrupt preempted
                                                                        its owner): the completion follows */
    q->active = 0;
    return q->pb.ioResult;
}

static void cancel(Q *q) {
    zero(&g.cpb, sizeof g.cpb);
    g.cpb.ioCRefNum = g.drv;
    g.cpb.csCode = MT_CS_DNR;
    g.cpb.op = MT_DNR_CANCEL;
    g.cpb.slot = q->pb.slot;
    g.cpb.name = (uint32_t)(uintptr_t)&q->pb;    /* identifies the pending PB */
    (void)PBControlSync((ParmBlkPtr)&g.cpb);
}

/* ---- the selectors ---- */

static long do_open(void) {
    short ref;
    OSErr e;
    if (g.open) return 0;
    e = OpenDriver((ConstStringPtr)"\p.IPP", &ref);            /* brings the driver up on the first open */
    if (e) return e;
    g.drv = ref;
    g.open = 1;
    return 0;
}

static long do_close(void) {
    int i;
    if (!g.open) return 0;
    g.open = 0;
    for (i = 0; i < NQ; i++)
        if (g.q[i].active) { g.q[i].active = 0; cancel(&g.q[i]); }   /* the result procs never run */
    if (!g.freed) {                              /* Retro68's .bss block (60 bytes): nothing here uses it */
        g.freed = 1;
        Retro68FreeGlobals();
    }
    return 0;
}

/* The name is all digits and dots and ends in a dot ("10.1.2.3."): the real DNR rejects it (D11). */
static int numeric_trailing_dot(const char *s) {
    int n = 0;
    for (; s[n]; n++)
        if (!((s[n] >= '0' && s[n] <= '9') || s[n] == '.')) return 0;
    return n > 0 && s[n - 1] == '.';
}

static long do_strtoaddr(const char *name, uint32_t hi, uint32_t proc, uint32_t ud) {
    if (!name || !hi) return E_DNRERR;
    if (!name[0]) { put_rtn(hi, 0); return 0; }  /* D15: an empty name answers 0 and nothing else */
    if (numeric_trailing_dot(name)) { put_rtn(hi, E_SYNTAX); return E_SYNTAX; }
    return lookup(MT_DNR_A, name, 0, hi, proc, ud);
}

static long do_addrtostr(uint32_t addr, char *out) {
    int i, n = 0;
    for (i = 3; i >= 0; i--) {
        unsigned v = (unsigned)((addr >> (8 * i)) & 255u);
        if (v >= 100u) out[n++] = (char)('0' + v / 100u);
        if (v >= 10u) out[n++] = (char)('0' + (v / 10u) % 10u);
        out[n++] = (char)('0' + v % 10u);
        if (i) out[n++] = '.';
    }
    out[n] = 0;
    return 0;                                    /* the real one leaves junk (4) in D0; callers ignore it */
}

static long do_enumcache(uint32_t proc, uint32_t ud) {
    uint32_t i;
    if (!proc) return 0;
    for (i = 0; i < 256; i++) {
        CacheRec r;
        zero(&g.cpb, sizeof g.cpb);
        g.cpb.ioCRefNum = g.drv;
        g.cpb.csCode = MT_CS_DNR;
        g.cpb.op = MT_DNR_ENUM;
        g.cpb.arg = i;
        g.cpb.out = (uint32_t)(uintptr_t)&g.en;
        if (PBControlSync((ParmBlkPtr)&g.cpb) || !g.cpb.slot) break;
        r.cname = (uint32_t)(uintptr_t)g.en.name;
        r.type = g.en.type;
        r.cclass = g.en.cclass;
        r.ttl = g.en.ttl == 0xFFFFFFFFu ? 0xFFFFFFFFu : LM_TIME + g.en.ttl;   /* absolute, as MacTCP's (D60) */
        r.rdata = g.en.rdata;
        mt_dnr_call2(proc, (uint32_t)(uintptr_t)&r, ud);
    }
    return 0;
}

/* The one C entry (mt_dnr_start.s jumps here with the caller's frame): sel, then up to four
 * arguments. Unused arguments read caller stack and are never used. */
long mt_dnr_entry(long sel, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    switch (sel) {
    case 0:           return 0;
    case S_OPEN:      return do_open();
    case S_CLOSE:     return do_close();
    case S_STRTOADDR: return do_strtoaddr((const char *)(uintptr_t)a, b, c, d);
    case S_ADDRTOSTR: return do_addrtostr(a, (char *)(uintptr_t)b);
    case S_ENUMCACHE: return do_enumcache(a, b);
    case S_ADDRTONAME:
        if (!b) return E_DNRERR;
        return lookup(MT_DNR_PTR, 0, a, b, c, d);
    case S_HINFO:
    case S_MXINFO:
        if (!a || !b) return E_DNRERR;
        return lookup(sel == S_HINFO ? MT_DNR_HINFO : MT_DNR_MX, (const char *)(uintptr_t)a, 0, b, c, d);
    default:          return -1;                 /* as the real one: out of range */
    }
}
