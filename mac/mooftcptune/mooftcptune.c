/* mooftcptune.c - MoofTCPTune: set MoofTCP's run-time timing levers (perf builds only).
 *
 * Writes MoofTCPTune-<id>.out beside the app (CR separated; MoofTCPTune.out without a run id):
 *   before: / after: lines with the levers as the driver reports them (mt_tune_abi.h),
 *   source: preset <id> | MoofTCPTune.cfg | none (the run id is no preset and there is no cfg: nothing is set),
 *   last line "RESULT OK" or "RESULT FAIL <reason>" (for example "not a tune build").
 * The levers apply to timers armed from then until the next restart. A run id that names a preset
 * (tune_parse.h) selects it; otherwise MoofTCPTune.cfg ("rto_min=", "rto_init=", "delack=", "stall=",
 * "quickack="; a missing key keeps the current value) is read if it exists.
 * A "!!<id>" run file (mac/common/runid.h) makes the run unattended: no alert, the app quits. */
#include <Quickdraw.h>
#include <Dialogs.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Memory.h>
#include <Files.h>
#include <Devices.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include "mt.h"
#include "mt_tune_abi.h"
#include "tune_parse.h"
#include "runid.h"
#pragma GCC diagnostic ignored "-Warray-bounds"   /* before mt_units.h: its low-memory reads are inlined */
#include "mt_units.h"

QDGlobals qd;

#define UTABLE_BASE (*(uint8_t ***volatile *)0x11C)
#define UNIT_COUNT  (*(volatile int16_t *)0x1D2)

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

/* 1 if the run file exists and starts with '!' (as MoofDiag: runid_name would otherwise ask in DLOG 200, which
 * this app has no resource for). */
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

/* Control MT_CS_TUNE get or set on the open .IPP; w is the 16-byte block. */
static OSErr tune_call(short ref, uint16_t *w, uint16_t op)
{
    CntrlParam pb;
    void *p = w;
    memset(&pb, 0, sizeof pb);
    w[MT_TW_VERSION] = MT_TUNE_ABI_VERSION;
    w[MT_TW_OP] = op;
    pb.ioCRefNum = ref;
    pb.csCode = MT_CS_TUNE;
    memcpy(&pb.csParam[0], &p, 4);
    return PBControlSync((ParmBlkPtr)&pb);
}

static void put_vals(const char *tag, const uint16_t *w)
{
    P("%s: rto_min=%u rto_init=%u delack=%u stall=%u quickack=%u\r", tag, (unsigned)w[MT_TW_RTO_MIN],
      (unsigned)w[MT_TW_RTO_INIT], (unsigned)w[MT_TW_DELACK], (unsigned)w[MT_TW_STALL], (unsigned)w[MT_TW_QUICKACK]);
}

static void to_block(const tn_vals *v, uint16_t *w)
{
    w[MT_TW_RTO_MIN] = (uint16_t)v->rto_min; w[MT_TW_RTO_INIT] = (uint16_t)v->rto_init;
    w[MT_TW_DELACK] = (uint16_t)v->delack; w[MT_TW_STALL] = (uint16_t)v->stall; w[MT_TW_QUICKACK] = (uint16_t)v->quickack;
}

static void from_block(const uint16_t *w, tn_vals *v)
{
    v->rto_min = w[MT_TW_RTO_MIN]; v->rto_init = w[MT_TW_RTO_INIT]; v->delack = w[MT_TW_DELACK];
    v->stall = w[MT_TW_STALL]; v->quickack = w[MT_TW_QUICKACK];
}

/* MoofTCPTune.cfg beside the app: its text in buf, length returned, or -1 if there is none. */
static long read_cfg(char *buf, long cap)
{
    short r;
    long len = cap;
    if (FSOpen((ConstStr255Param)"\pMoofTCPTune.cfg", 0, &r) != noErr) return -1;
    if (FSRead(r, &len, buf) != noErr && len <= 0) len = 0;
    FSClose(r);
    return len;
}

int main(void)
{
    char line[160], id[40], reason[80], cfg[513], err[40];
    Str255 ps;
    unsigned char oname[40];
    uint16_t before[MT_TW_N], after[MT_TW_N], want[MT_TW_N];
    tn_vals v;
    size_t n;
    short ref = 0;
    OSErr r;
    int ok = 0, i;
    long clen;

    MaxApplZone();
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(0);
    InitCursor();
    if (has_run_file()) runid_name("MoofTCPTune", ".out", oname);
    else memcpy(oname, "\x0fMoofTCPTune.out", 16);
    Create((ConstStringPtr)oname, 0, 'ttxt', 'TEXT');
    if (FSOpen((ConstStringPtr)oname, 0, &outref)) outref = -1;
    else SetEOF(outref, 0);
    P("MoofTCPTune\r");
    /* the run id is the output name between "MoofTCPTune-" and ".out" */
    id[0] = 0;
    if (oname[0] > 16 && oname[12] == '-') {
        n = (size_t)oname[0] - 16;
        memcpy(id, oname + 13, n);
        id[n] = 0;
    }
    strcpy(reason, "");

    {
        const uint8_t *dce = NULL, *drv;
        r = OpenDriver((ConstStringPtr)"\p.IPP", &ref);
        drv = r == 0 ? mt_units_find_ipp_dce(&dce) : NULL;
        if (r != 0) sprintf(reason, "OpenDriver(.IPP) %d", (int)r);
        else if (!drv || !mt_units_is_moof(drv)) strcpy(reason, "stack is not MoofTCP");  /* never send our private Control to MacTCP */
    }
    if (!reason[0]) {
        memset(before, 0, sizeof before);
        r = tune_call(ref, before, MT_TUNE_OP_GET);
        if (r != 0) sprintf(reason, "not a tune build (Control %d)", (int)r);
        else put_vals("before", before);
    }
    if (!reason[0]) {
        from_block(before, &v);
        if (id[0] && tn_preset(id, &v)) P("source: preset %s\r", id);
        else if ((clen = read_cfg(cfg, 512)) >= 0) {
            int k = tn_parse_cfg(cfg, clen, &v, err, (int)sizeof err);
            if (k < 0) sprintf(reason, "MoofTCPTune.cfg: %s", err);
            else P("source: MoofTCPTune.cfg (%d keys)\r", k);
        } else P("source: none (run id '%s' is no preset, no MoofTCPTune.cfg)\r", id);
    }
    if (!reason[0]) {
        memcpy(want, before, sizeof want);
        to_block(&v, want);
        r = tune_call(ref, want, MT_TUNE_OP_SET);
        if (r != 0) sprintf(reason, "driver refused the values (Control %d)", (int)r);
    }
    if (!reason[0]) {
        memset(after, 0, sizeof after);
        r = tune_call(ref, after, MT_TUNE_OP_GET);
        if (r != 0) sprintf(reason, "read back failed (Control %d)", (int)r);
        else {
            put_vals("after", after);
            to_block(&v, want);
            for (i = MT_TW_RTO_MIN; i <= MT_TW_QUICKACK; i++)
                if (after[i] != want[i]) strcpy(reason, "values read back differ from those set");
        }
    }
    ok = reason[0] == 0;
    if (ok) P("RESULT OK\r");
    else P("RESULT FAIL %s\r", reason);
    if (outref >= 0) { FSClose(outref); outref = -1; }
    FlushVol(0, 0);
    if (runid_unattended()) return 0;
    sprintf(line, "MoofTCPTune %s. Results are in %.*s", ok ? "finished" : "FAIL", (int)oname[0], (char *)oname + 1);
    n = strlen(line);
    ps[0] = (unsigned char)n;
    memcpy(ps + 1, line, n);
    ParamText(ps, (ConstStringPtr)"\p", (ConstStringPtr)"\p", (ConstStringPtr)"\p");
    Alert(128, 0);
    return 0;
}
