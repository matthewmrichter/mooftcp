/* kl_os.c - Toolbox wrappers for KeyLat (see kl_os.h). */
#pragma GCC diagnostic ignored "-Wpointer-sign"
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
#include <Traps.h>
#include <string.h>
#include "kl_os.h"

QDGlobals qd;
static short outref = -1;
static WindowPtr win;
static char lines[6][96];
static int have_wne = -1;

static int trap_avail(uint16_t trap)
{
    ProcPtr unimp = NGetTrapAddress(0xA89F, kToolboxTrapType);
    return NGetTrapAddress(trap, (trap & 0x0800) ? kToolboxTrapType : kOSTrapType) != unimp;
}

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
    FlushEvents(everyEvent, 0);
    have_wne = trap_avail(0xA860);
}

uint32_t os_ticks(void) { return (uint32_t)TickCount(); }
short os_open_ipp(short *ref) { return (short)OpenDriver("\p.IPP", ref); }
short os_control_sync(void *pb) { return (short)PBControlSync((ParmBlkPtr)pb); }
short os_control_async(void *pb) { return (short)PBControlAsync((ParmBlkPtr)pb); }

static void get_text(DialogPtr d, short item, char *out, int max)
{
    short type; Handle h; Rect r; Str255 s; int n;
    GetDialogItem(d, item, &type, &h, &r);
    GetDialogItemText(h, s);
    n = s[0] < max - 1 ? s[0] : max - 1;
    memcpy(out, s + 1, (size_t)n);
    out[n] = 0;
}

static void set_text(DialogPtr d, short item, const char *c)
{
    short type; Handle h; Rect r; Str255 s; size_t n = strlen(c);
    if (n > 250) n = 250;
    s[0] = (unsigned char)n;
    memcpy(s + 1, c, n);
    GetDialogItem(d, item, &type, &h, &r);
    SetDialogItemText(h, s);
}

int os_setup_dialog(char *host, int hmax, char *port, int pmax)
{
    DialogPtr d = GetNewDialog(128, 0, (WindowPtr)-1);
    short item = 0;
    if (!d) return 1;                       /* no dialog resource: connect with the prefilled values */
    SetPort((GrafPtr)d);
    set_text(d, 4, host);
    set_text(d, 6, port);
    SelectDialogItemText(d, 4, 0, 32767);
    ShowWindow((WindowPtr)d);
    do { ModalDialog(0, &item); } while (item != 1 && item != 2);
    if (item == 1) { get_text(d, 4, host, hmax); get_text(d, 6, port, pmax); }
    DisposeDialog(d);
    return item == 1;
}

static void draw_line(int i)
{
    Rect r;
    SetRect(&r, 4, 8 + i * 18, 506, 8 + i * 18 + 18);
    EraseRect(&r);
    MoveTo(8, 8 + i * 18 + 13);
    DrawText(lines[i], 0, (short)strlen(lines[i]));
}

void os_window(void)
{
    Rect r;
    SetRect(&r, 8, 44, 504, 8 + 6 * 18 + 56);
    win = NewWindow(0, &r, "\pKeyLat", true, noGrowDocProc, (WindowPtr)-1, true, 0);
    SetPort(win);
    TextFont(4); TextSize(9);
}

void os_show(int line, const char *s)
{
    if (line < 0 || line > 5) return;
    strncpy(lines[line], s, sizeof lines[line] - 1);
    lines[line][sizeof lines[line] - 1] = 0;
    if (win) { SetPort(win); draw_line(line); }
}

void os_alert(const char *s)
{
    Str255 p; size_t n = strlen(s);
    if (n > 200) n = 200;
    p[0] = (unsigned char)n;
    memcpy(p + 1, s, n);
    ParamText(p, "\p", "\p", "\p");
    (void)Alert(128, 0);
}

int os_event_pending(void)
{
    EventRecord ev;
    return EventAvail(everyEvent, &ev) ? 1 : 0;
}

int os_poll(KlKey *k)
{
    EventRecord ev;
    Boolean got;
    if (have_wne) got = WaitNextEvent(everyEvent, &ev, 0, 0);
    else { SystemTask(); got = GetNextEvent(everyEvent, &ev); }
    if (!got) return 0;
    switch (ev.what) {
    case keyDown:
        k->seen = (uint32_t)TickCount();
        k->when = (uint32_t)ev.when;
        k->ch = (char)(ev.message & charCodeMask);
        if (ev.modifiers & cmdKey) return ((ev.message & charCodeMask) | 0x20) == 'q' ? 2 : 0;
        return 1;
    case updateEvt:
        if ((WindowPtr)ev.message == win) {
            int i;
            BeginUpdate(win);
            SetPort(win);
            for (i = 0; i < 6; i++) draw_line(i);
            EndUpdate(win);
        }
        return 0;
    case mouseDown: {
        WindowPtr w;
        short part = FindWindow(ev.where, &w);
        if (part == inSysWindow) SystemClick(&ev, w);
        else if (part == inDrag && w == win) DragWindow(w, ev.where, &qd.screenBits.bounds);
        else if (part == inGoAway && w == win && TrackGoAway(w, ev.where)) return 2;
        else if (part == inContent && w != FrontWindow()) SelectWindow(w);
        return 0;
    }
    default:
        return 0;
    }
}

short os_out_open(void)
{
    OSErr e;
    Create("\pKeyLat.out", 0, 'ttxt', 'TEXT');
    e = FSOpen("\pKeyLat.out", 0, &outref);
    if (e) { outref = -1; return (short)e; }
    SetEOF(outref, 0);
    return 0;
}

short os_out_write(const char *buf, long n)
{
    long cnt = n;
    short e;
    if (outref < 0) return -1;
    e = (short)FSWrite(outref, &cnt, buf);
    FlushVol(0, 0);
    return e;
}

void os_out_close(void)
{
    if (outref >= 0) { FSClose(outref); FlushVol(0, 0); outref = -1; }
}

int os_read_cfg(char *buf, int max)
{
    short ref;
    long cnt = max;
    if (FSOpen("\pKeyLat.cfg", 0, &ref)) return 0;
    if (FSRead(ref, &cnt, buf) && cnt == 0) { FSClose(ref); return 0; }
    FSClose(ref);
    return (int)cnt;
}

void os_write_cfg(const char *buf, long n)
{
    short ref;
    long cnt = n;
    Create("\pKeyLat.cfg", 0, 'ttxt', 'TEXT');
    if (FSOpen("\pKeyLat.cfg", 0, &ref)) return;
    SetEOF(ref, 0);
    FSWrite(ref, &cnt, buf);
    FSClose(ref);
    FlushVol(0, 0);
}
