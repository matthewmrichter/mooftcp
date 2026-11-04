/* probe_os.c - Toolbox wrappers for ApiProbe (see probe_os.h). */
/* Pascal string literals are char *, the Toolbox wants unsigned char *; low-memory globals sit at fixed small addresses. */
#pragma GCC diagnostic ignored "-Wpointer-sign"
#pragma GCC diagnostic ignored "-Warray-bounds"
#include <Quickdraw.h>
#include <Dialogs.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Memory.h>
#include <Events.h>
#include <OSUtils.h>
#include <Files.h>
#include <Devices.h>
#include <Resources.h>
#include <string.h>
#include "probe_os.h"

QDGlobals qd;
static short outref = -1;

void os_init(void)
{
    MaxApplZone();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(0);
    InitCursor();
}

unsigned long os_ticks(void) { return (unsigned long)TickCount(); }

short os_open_ipp(short *ref) { return (short)OpenDriver("\p.IPP", ref); }
short os_close_drv(short ref) { return (short)CloseDriver(ref); }
short os_control_sync(void *pb) { return (short)PBControlSync((ParmBlkPtr)pb); }
short os_control_async(void *pb) { return (short)PBControlAsync((ParmBlkPtr)pb); }
unsigned long os_cur_a5(void) { return *(unsigned long *)0x904; }
unsigned long os_free_mem(void) { return (unsigned long)FreeMem(); }

short os_out_open(const char *pname)
{
    OSErr e;
    Create((ConstStr255Param)pname, 0, 'ttxt', 'TEXT');
    e = FSOpen((ConstStr255Param)pname, 0, &outref);
    if (e) { outref = -1; return (short)e; }
    SetEOF(outref, 0);
    return 0;
}

short os_out_write(const char *buf, long n)
{
    long cnt = n;
    if (outref < 0) return -1;
    return (short)FSWrite(outref, &cnt, buf);
}

void os_out_flush(void)
{
    if (outref >= 0) FlushVol(0, 0);
}

void os_out_close(void)
{
    if (outref >= 0) { FSClose(outref); FlushVol(0, 0); outref = -1; }
}

void os_done_alert(void)
{
    Alert(128, 0);
}

static void set_text(DialogPtr d, short item, const char *c)
{
    short type; Handle h; Rect r; Str255 s; size_t n = strlen(c);
    if (n > 250) n = 250;
    s[0] = (unsigned char)n; memcpy(s + 1, c, n);
    GetDialogItem(d, item, &type, &h, &r);
    SetDialogItemText(h, s);
}

static void get_text(DialogPtr d, short item, char *out, int max)
{
    short type; Handle h; Rect r; Str255 s; int n;
    GetDialogItem(d, item, &type, &h, &r);
    GetDialogItemText(h, s);
    n = s[0] < max - 1 ? s[0] : max - 1;
    memcpy(out, s + 1, (size_t)n);
    out[n] = 0;
}

/* Cmd-Q in the dialog = Quit (item 2) */
static pascal Boolean ask_filter(DialogPtr d, EventRecord *ev, short *item)
{
    (void)d;   /* a filter replaces ModalDialog's standard one, so Return/Enter = OK is ours to do */
    if ((ev->what == keyDown || ev->what == autoKey) && !(ev->modifiers & cmdKey)
        && ((ev->message & charCodeMask) == 0x0D || (ev->message & charCodeMask) == 0x03)) { *item = 1; return true; }
    if (ev->what == keyDown && (ev->modifiers & cmdKey) && ((ev->message & charCodeMask) | 0x20) == 'q') { *item = 2; return true; }
    return false;
}

int os_ask(char *ip, int ipmax, char *groups, int gmax, const char *(*check)(char *ip, char *groups))
{
    DialogPtr d = GetNewDialog(129, 0, (WindowPtr)-1);
    short item = 0, ok = 0;
    if (!d) return 1;                       /* no dialog resource: go on with the prefilled values */
    SetPort((GrafPtr)d);
    set_text(d, 4, ip);
    set_text(d, 6, groups);
    set_text(d, 8, "Return = OK. Cmd-Q = Quit.");
    SelectDialogItemText(d, 4, 0, 32767);
    ShowWindow((WindowPtr)d);
    while (!ok) {
        do { ModalDialog(ask_filter, &item); } while (item != 1 && item != 2);
        if (item == 2) break;
        get_text(d, 4, ip, ipmax); get_text(d, 6, groups, gmax);
        {   /* the message goes into the dialog itself (item 8); no second window */
            const char *msg = check ? check(ip, groups) : 0;
            if (!msg) ok = 1;
            else {
                short t; Handle h; Rect r;
                GetDialogItem(d, 8, &t, &h, &r);
                set_text(d, 8, msg);
                EraseRect(&r); InvalRect(&r);   /* SetDialogItemText does not redraw by itself */
                SelectDialogItemText(d, 4, 0, 32767);
            }
        }
    }
    DisposeDialog(d);
    return ok;
}

void os_write_file(const char *pname, const char *buf, long n)
{
    short ref; long cnt = n;
    Create((ConstStr255Param)pname, 0, 'ttxt', 'TEXT');
    if (FSOpen((ConstStr255Param)pname, 0, &ref)) return;
    SetEOF(ref, 0);
    FSWrite(ref, &cnt, buf);
    FSClose(ref);
    FlushVol(0, 0);
}

int os_read_cfg(const char *pname, char *buf, int max)
{
    short ref; long cnt = max;
    if (FSOpen((ConstStr255Param)pname, 0, &ref)) return 0;
    if (FSRead(ref, &cnt, buf) && cnt == 0) { FSClose(ref); return 0; }
    FSClose(ref);
    return (int)cnt;
}

static Handle dnr_h;

void *os_load_dnr(short *err)
{
    SysEnvRec world;
    short ref;
    Handle code = 0;
    *err = 0;
    SysEnvirons(1, &world);
    ref = OpenRFPerm("\pMacTCP DNR", world.sysVRefNum, fsRdPerm);
    if (ref == -1) ref = OpenRFPerm("\pMoofTCP", world.sysVRefNum, fsRdPerm);   /* the release has no "MacTCP DNR" */
    if (ref == -1) { *err = (short)ResError(); if (!*err) *err = -1; return 0; }
    code = Get1IndResource('dnrp', 1);
    if (code) DetachResource(code); else *err = -2;
    CloseResFile(ref);
    if (!code) return 0;
    HLock(code);
    dnr_h = code;
    return *code;
}

void os_free_dnr(void *code)
{
    (void)code;
    if (dnr_h) { HUnlock(dnr_h); DisposeHandle(dnr_h); dnr_h = 0; }
}

unsigned long os_app_free(void)
{
    THz saved = GetZone(); long f;
    SetZone(ApplicationZone()); f = FreeMem(); SetZone(saved); return (unsigned long)f;
}

unsigned long os_sys_free(void)
{
    THz saved = GetZone(); long f;
    SetZone(*(THz *)0x2A6); f = FreeMem(); SetZone(saved); return (unsigned long)f;
}
