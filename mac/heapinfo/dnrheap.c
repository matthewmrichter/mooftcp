/* HeapInfoDNR: measure MacTCP DNR memory from inside an application.
 * Loads the 'dnrp' code resource from "MacTCP DNR" in the System Folder itself
 * (own glue, no Apple source), then records system- and application-zone FreeMem
 * at: start, after OpenResolver, after a successful StrToAddr, while still open,
 * and after CloseResolver (and disposing the code). */
#include <Quickdraw.h>
#include <Dialogs.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Memory.h>
#include <Events.h>
#include <Resources.h>
#include <OSUtils.h>
#include <Files.h>

QDGlobals qd;

#define OPENRESOLVER  1L
#define CLOSERESOLVER 2L
#define STRTOADDR     3L
#define CACHEFAULT    (-23042)

struct hostInfo {
    long rtnCode;
    char cname[255];
    unsigned long addr[4];
};

typedef long (*DnrProc)(long selector, ...);

static pascal void doneProc(struct hostInfo *hi, char *ud)
{
    (void)hi;
    *(volatile short *)ud = 1;   /* no globals: callable without our A5 */
}

static long sysFree(void)
{
    THz saved = GetZone(); long f;
    SetZone(*(THz *)0x2A6); f = FreeMem(); SetZone(saved); return f;
}
static long appFree(void)
{
    THz saved = GetZone(); long f;
    SetZone(ApplicationZone()); f = FreeMem(); SetZone(saved); return f;
}

static void put(unsigned char *out, int *i, const char *s) { while (*s) out[++*i] = (unsigned char)*s++; }
static void num(unsigned char *out, int *i, long v)
{
    char tmp[16]; int n = 0; unsigned long u;
    if (v < 0) { out[++*i] = '-'; u = (unsigned long)(-v); } else u = (unsigned long)v;
    if (u == 0) tmp[n++] = '0';
    while (u) { tmp[n++] = '0' + (char)(u % 10); u /= 10; }
    while (n) out[++*i] = (unsigned char)tmp[--n];
}
/* "\r<label><a> / <b>" */
static void line2(const char *label, long a, long b, unsigned char *out)
{
    int i = 0;
    out[++i] = 13; put(out, &i, label); num(out, &i, a); put(out, &i, " / "); num(out, &i, b);
    out[0] = (unsigned char)i;
}

int main(void)
{
    static const char host[] = "172.17.0.1.nip.io";
    SysEnvRec world;
    Handle code = 0;
    DnrProc dnr;
    short ref, rc, done = 0;
    long rcOpen = -1, rcLook = -1, s0, a0, s1, a1, s2, a2, s3, a3, t;
    struct hostInfo hi;
    unsigned char l1[64], l2[64], l3[64], l4[64];
    unsigned long ad;
    int i;

    MaxApplZone();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(0);
    InitCursor();

    s0 = sysFree(); a0 = appFree();
    s1 = a1 = s2 = a2 = s3 = a3 = 0;
    hi.rtnCode = 0; hi.addr[0] = 0;

    SysEnvirons(1, &world);
    ref = OpenRFPerm("\pMacTCP DNR", world.sysVRefNum, fsRdPerm);
    if (ref != -1) {
        code = Get1IndResource('dnrp', 1);
        if (code) DetachResource(code);
        CloseResFile(ref);
    }
    if (code) {
        HLock(code);
        dnr = (DnrProc)*code;
        rcOpen = (short)dnr(OPENRESOLVER, (char *)0);
        s1 = sysFree(); a1 = appFree();
        if (rcOpen == 0) {
            rc = (short)dnr(STRTOADDR, (char *)host, &hi, doneProc, (char *)&done);
            if (rc == CACHEFAULT) {
                t = TickCount() + 60L * 20;
                while (!done && TickCount() < t) SystemTask();
                rcLook = done ? hi.rtnCode : 99;
            } else rcLook = rc;
            s2 = sysFree(); a2 = appFree();
            dnr(CLOSERESOLVER);
        }
        HUnlock(code);
        DisposeHandle(code);
        s3 = sysFree(); a3 = appFree();
    }

    /* Alert 1: results and absolute starting values */
    line2("open/lookup err: ", rcOpen, rcLook, l1);
    i = 0; l2[++i] = 13; put(l2, &i, "addr: ");
    ad = hi.addr[0];
    num(l2, &i, (long)((ad >> 24) & 255)); put(l2, &i, ".");
    num(l2, &i, (long)((ad >> 16) & 255)); put(l2, &i, ".");
    num(l2, &i, (long)((ad >> 8) & 255)); put(l2, &i, ".");
    num(l2, &i, (long)(ad & 255)); l2[0] = (unsigned char)i;
    line2("start sys/app free: ", s0, a0, l3);
    line2("open d sys/app: ", s0 - s1, a0 - a1, l4);
    ParamText(l1, l2, l3, l4);
    Alert(128, 0);
    /* Alert 2: occupied deltas (bytes taken from each zone vs start) */
    line2("looked-up d sys/app: ", s0 - s2, a0 - a2, l1);
    line2("closed d sys/app: ", s0 - s3, a0 - a3, l2);
    line2("end sys/app free: ", s3, a3, l3);
    i = 0; l4[++i] = 13; put(l4, &i, "host: "); put(l4, &i, host); l4[0] = (unsigned char)i;
    ParamText(l1, l2, l3, l4);
    Alert(128, 0);
    return 0;
}
